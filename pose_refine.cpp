#include "pose_refine.h"
#include <omp.h>

static void accumBilateral(long delta, long i, long j, long *A, long *b, int threshold)
{
    long f = std::abs(delta) < threshold ? 1 : 0;

    const long fi = f * i;
    const long fj = f * j;

    A[0] += fi * i;
    A[1] += fi * j;
    A[3] += fj * j;
    b[0] += fi * delta;
    b[1] += fj * delta;
}

static void cannyTraceEdge(int rowOffset, int colOffset, int row, int col, cv::Mat& canny_edge, cv::Mat& mag_nms){
    int newRow = row + rowOffset;
    int newCol = col + colOffset;

    if(newRow>0 && newRow<mag_nms.rows && newCol>0 && newCol<mag_nms.cols){
        float mag_v = mag_nms.at<float>(newRow, newCol);
        if(canny_edge.at<uchar>(newRow, newCol)>0 || mag_v < 0.01f) return ;

        canny_edge.at<uchar>(newRow, newCol) = 255;
        cannyTraceEdge ( 1, 0, newRow, newCol, canny_edge, mag_nms);
        cannyTraceEdge (-1, 0, newRow, newCol, canny_edge, mag_nms);
        cannyTraceEdge ( 1, 1, newRow, newCol, canny_edge, mag_nms);
        cannyTraceEdge (-1, -1, newRow, newCol, canny_edge, mag_nms);
        cannyTraceEdge ( 0, -1, newRow, newCol, canny_edge, mag_nms);
        cannyTraceEdge ( 0, 1, newRow, newCol, canny_edge, mag_nms);
        cannyTraceEdge (-1, 1, newRow, newCol, canny_edge, mag_nms);
        cannyTraceEdge ( 1, -1, newRow, newCol, canny_edge, mag_nms);
    }
};

PoseRefine::PoseRefine(cv::Mat depth, cv::Mat K, std::string model_path): model(model_path)
{
    assert(depth.type() == CV_16U && K.type() == CV_32F);

    scene_depth = depth;
    this->K = K;
    scene_dep_edge = get_depth_edge(depth, K);
    width = depth.cols;
    height = depth.rows;

    proj_mat = cuda_renderer::compute_proj(K, width, height);

#ifdef CUDA_ON
    scene.init_Scene_projective_cuda(scene_depth, reinterpret_cast<Mat3x3f&>(K),
                                     pcd_buffer_cuda, normal_buffer_cuda);
#else
    scene.init_Scene_projective_cpu(scene_depth, reinterpret_cast<Mat3x3f&>(K), pcd_buffer, normal_buffer);
#endif
}

std::vector<cuda_icp::RegistrationResult> PoseRefine::process_batch(std::vector<cv::Mat>& init_poses,
                                                                    int down_sample,
                                                                    bool depth_aligned)
{
    std::vector<cuda_icp::RegistrationResult> result_poses(init_poses.size());
    cuda_icp::ICPConvergenceCriteria criteria;

    assert(width%down_sample == 0 && height%down_sample == 0);
    const int width_local = width/down_sample;
    const int height_local = height/down_sample;

    Mat3x3f K_icp((float*)K.data); // ugly but useful
    K_icp[0][0] /= down_sample; K_icp[1][1] /= down_sample;
    K_icp[0][2] /= down_sample; K_icp[1][2] /= down_sample;

    std::vector<cuda_renderer::Model::mat4x4> mat4_v(batch_size);

    float temp_max_diff = scene.max_dist_diff;
    auto setting_for_align = [&](){
        criteria = {1e-5f, 1e-5f, 1};   // just 1 iteration
        scene.max_dist_diff = FLT_MAX;  // invalidate rejection
    };
    auto setting_backup = [&](){
        criteria = {1e-5f, 1e-5f, 30};
        scene.max_dist_diff = temp_max_diff;
    };

    auto icp_batch = [&](int i){
        // directly down sample by viewport
#ifdef CUDA_ON
        auto depths = cuda_renderer::render_cuda_keep_in_gpu(model.tris, mat4_v,
                                                           width_local, height_local, proj_mat);
#else
        auto depths = cuda_renderer::render_cpu(model.tris, mat4_v,
                                                           width_local, height_local, proj_mat);
#endif
        // cuda per thread stream
# pragma omp parallel num_threads(mat4_v.size())
        {
            int j = omp_get_thread_num();
            result_poses[j+i].transformation_ = reinterpret_cast<Mat4x4f&>(mat4_v[j]);
#ifdef CUDA_ON
            auto pcd1_cuda = cuda_icp::depth2cloud_cuda(depths.data() + j*width_local*height_local,
                                                        width_local, height_local, K_icp);
#else
            auto pcd1_cpu = cuda_icp::depth2cloud_cpu(depths.data() + j*width_local*height_local,
                                                        width_local, height_local, K_icp);
#endif
            Mat4x4f previous_T;
            if(!depth_aligned){
                setting_for_align();
                previous_T = result_poses[j+i].transformation_;
#ifdef CUDA_ON
                result_poses[j+i] = cuda_icp::ICP_Point2Plane_cuda(pcd1_cuda, scene, criteria);
#else
                result_poses[j+i] = cuda_icp::ICP_Point2Plane_cpu(pcd1_cpu, scene, criteria);
#endif
                result_poses[j+i].transformation_ = result_poses[j+i].transformation_ * previous_T;
            }

            setting_backup();
            previous_T = result_poses[j+i].transformation_;
#ifdef CUDA_ON
            result_poses[j+i] = cuda_icp::ICP_Point2Plane_cuda(pcd1_cuda, scene, criteria);
#else
            result_poses[j+i] = cuda_icp::ICP_Point2Plane_cpu(pcd1_cpu, scene, criteria);
#endif
            result_poses[j+i].transformation_ = result_poses[j+i].transformation_ * previous_T;
        }
    };

    int i=0;
    for(; i<=init_poses.size()-batch_size; i+=batch_size){
        for(int j=0; j<batch_size; j++) mat4_v[j].init_from_cv(init_poses[j+i]);
        icp_batch(i);
    }

    int left = init_poses.size() - i;

    if(left > 0){
        mat4_v.resize(left);
        for(int j=0; j<left; j++) mat4_v[j].init_from_cv(init_poses[j+i]);
        icp_batch(i);
    }

    return result_poses;
}

cv::Mat PoseRefine::get_normal(cv::Mat &depth__, cv::Mat K)
{
    cv::Mat depth;
    int depth_type = depth__.type();
    assert(depth_type == CV_16U || depth_type == CV_32S);
    if(depth_type == CV_32S){
        depth__.convertTo(depth, CV_16U);
    }else{
        depth = depth__;
    }

    float fx = 530;
    float fy = 530;
    if(!K.empty()){
        assert(K.type() == CV_32F);
        fx = K.at<float>(0, 0);
        fy = K.at<float>(1, 1);
    }

//       cv::medianBlur(depth, depth, 5);
       cv::Mat normals = cv::Mat(depth.size(), CV_32FC3, cv::Scalar(0));
       // method from linemod depth modality
       {
           cv::Mat src = depth;
           int distance_threshold = 2000;
           int difference_threshold = 50;

           const unsigned short *lp_depth = src.ptr<ushort>();
           cv::Vec3f *lp_normals = normals.ptr<cv::Vec3f>();

           const int l_W = src.cols;
           const int l_H = src.rows;

           const int l_r = 5; // used to be 7
           const int l_offset0 = -l_r - l_r * l_W;
           const int l_offset1 = 0 - l_r * l_W;
           const int l_offset2 = +l_r - l_r * l_W;
           const int l_offset3 = -l_r;
           const int l_offset4 = +l_r;
           const int l_offset5 = -l_r + l_r * l_W;
           const int l_offset6 = 0 + l_r * l_W;
           const int l_offset7 = +l_r + l_r * l_W;

           for (int l_y = l_r; l_y < l_H - l_r - 1; ++l_y)
           {
               const unsigned short *lp_line = lp_depth + (l_y * l_W + l_r);
               cv::Vec3f *lp_norm = lp_normals + (l_y * l_W + l_r);

               for (int l_x = l_r; l_x < l_W - l_r - 1; ++l_x)
               {
                   long l_d = lp_line[0];
                   if (l_d < distance_threshold /*&& l_d > 0*/)
                   {
                       // accum
                       long l_A[4];
                       l_A[0] = l_A[1] = l_A[2] = l_A[3] = 0;
                       long l_b[2];
                       l_b[0] = l_b[1] = 0;
                       accumBilateral(lp_line[l_offset0] - l_d, -l_r, -l_r, l_A, l_b, difference_threshold);
                       accumBilateral(lp_line[l_offset1] - l_d, 0, -l_r, l_A, l_b, difference_threshold);
                       accumBilateral(lp_line[l_offset2] - l_d, +l_r, -l_r, l_A, l_b, difference_threshold);
                       accumBilateral(lp_line[l_offset3] - l_d, -l_r, 0, l_A, l_b, difference_threshold);
                       accumBilateral(lp_line[l_offset4] - l_d, +l_r, 0, l_A, l_b, difference_threshold);
                       accumBilateral(lp_line[l_offset5] - l_d, -l_r, +l_r, l_A, l_b, difference_threshold);
                       accumBilateral(lp_line[l_offset6] - l_d, 0, +l_r, l_A, l_b, difference_threshold);
                       accumBilateral(lp_line[l_offset7] - l_d, +l_r, +l_r, l_A, l_b, difference_threshold);

                       // solve
                       long l_det = l_A[0] * l_A[3] - l_A[1] * l_A[1];
                       long l_ddx = l_A[3] * l_b[0] - l_A[1] * l_b[1];
                       long l_ddy = -l_A[1] * l_b[0] + l_A[0] * l_b[1];

                       float l_nx = static_cast<float>(fx * l_ddx);
                       float l_ny = static_cast<float>(fy * l_ddy);
                       float l_nz = static_cast<float>(-l_det * l_d);

                       float l_sqrt = sqrtf(l_nx * l_nx + l_ny * l_ny + l_nz * l_nz);

                       if (l_sqrt > 0)
                       {
                           float l_norminv = 1.0f / (l_sqrt);

                           l_nx *= l_norminv;
                           l_ny *= l_norminv;
                           l_nz *= l_norminv;

                           *lp_norm = {l_nx, l_ny, l_nz};
                       }
                   }
                   ++lp_line;
                   ++lp_norm;
               }
           }
       }
       return normals;
}

cv::Mat PoseRefine::get_depth_edge(cv::Mat &depth__, cv::Mat K)
{
    cv::Mat depth;
    int depth_type = depth__.type();
    assert(depth_type == CV_16U || depth_type == CV_32S);
    if(depth_type == CV_32S){
        depth__.convertTo(depth, CV_16U);
    }else{
        depth = depth__;
    }

    float fx = 530;
    float fy = 530;
    if(!K.empty()){
        assert(K.type() == CV_32F);
        fx = K.at<float>(0, 0);
        fy = K.at<float>(1, 1);
    }

    cv::medianBlur(depth, depth, 5);
    cv::Mat normals = get_normal(depth, K);

    cv::Mat N_xyz[3];
    cv::split(normals, N_xyz);

    // refer to RGB-D Edge Detection and Edge-based Registration
    // PCL organized edge detection
    // canny on normal
    cv::Mat sx, sy, mag;

    cv::GaussianBlur(N_xyz[0], N_xyz[0], {3, 3}, 1);
    cv::Sobel(N_xyz[0], sx, CV_32F, 1, 0, 3);
    cv::GaussianBlur(N_xyz[1], N_xyz[0], {3, 3}, 1);
    cv::Sobel(N_xyz[1], sy, CV_32F, 0, 1, 3);

    mag = sx.mul(sx) + sy.mul(sy);
    cv::sqrt(mag, mag);
    cv::medianBlur(mag, mag, 5);

    cv::Mat angle;
    cv::phase(sx, sy, angle, true);

    cv::Mat_<unsigned char> quantized_unfiltered;
    // method from linemod quantizing orientation
    {
        angle.convertTo(quantized_unfiltered, CV_8U, 8.0 / 360.0);

        // Zero out top and bottom rows
        /// @todo is this necessary, or even correct?
        memset(quantized_unfiltered.ptr(), 0, quantized_unfiltered.cols);
        memset(quantized_unfiltered.ptr(quantized_unfiltered.rows - 1), 0, quantized_unfiltered.cols);
        // Zero out first and last columns
        for (int r = 0; r < quantized_unfiltered.rows; ++r)
        {
            quantized_unfiltered(r, 0) = 0;
            quantized_unfiltered(r, quantized_unfiltered.cols - 1) = 0;
        }

        // Mask 8 buckets into 4 quantized orientations
        for (int r = 1; r < angle.rows - 1; ++r)
        {
            uchar *quant_r = quantized_unfiltered.ptr<uchar>(r);
            for (int c = 1; c < angle.cols - 1; ++c)
            {
                quant_r[c] &= 3;
            }
        }
    }

    float tLow = 0.2f;
    float tHigh = 1.1f;
    int max_search_neighbors_ = 50;
    cv::Mat mag_nms = cv::Mat(mag.size(), CV_32FC1, cv::Scalar(0));
    for(int r=1; r<mag.rows; r++){
        for(int c=1; c<mag.cols; c++){
            float mag_v = mag.at<float>(r, c);
            float& mag_nms_v = mag_nms.at<float>(r, c);
            if(mag_v<tLow) continue;

            uchar quant_angle = quantized_unfiltered.at<uchar>(r, c);
            if(quant_angle == 0){
                if(mag_v >= mag.at<float>(r, c+1) && mag_v >= mag.at<float>(r, c-1))
                    mag_nms_v = mag_v;
            }else if(quant_angle == 1){
                if(mag_v >= mag.at<float>(r-1, c+1) && mag_v >= mag.at<float>(r+1, c-1))
                    mag_nms_v = mag_v;
            }else if(quant_angle == 2){
                if(mag_v >= mag.at<float>(r-1, c) && mag_v >= mag.at<float>(r+1, c))
                    mag_nms_v = mag_v;
            }else if(quant_angle == 3){
                if(mag_v >= mag.at<float>(r+1, c+1) && mag_v >= mag.at<float>(r-1, c-1))
                    mag_nms_v = mag_v;
            }
        }
    }

    cv::Mat canny_edge = cv::Mat(mag_nms.size(), CV_8UC1, cv::Scalar(0));

    for(int r=0; r<canny_edge.rows; r++){
        for(int c=0; c<canny_edge.cols; c++){
            if(mag_nms.at<float>(r, c) < tHigh || canny_edge.at<uchar>(r, c)>0) continue;
            canny_edge.at<uchar>(r, c) = 255;

            cannyTraceEdge ( 1, 0, r, c, canny_edge, mag_nms);
            cannyTraceEdge (-1, 0, r, c, canny_edge, mag_nms);
            cannyTraceEdge ( 1, 1, r, c, canny_edge, mag_nms);
            cannyTraceEdge (-1, -1, r, c, canny_edge, mag_nms);
            cannyTraceEdge ( 0, -1, r, c, canny_edge, mag_nms);
            cannyTraceEdge ( 0, 1, r, c, canny_edge, mag_nms);
            cannyTraceEdge (-1, 1, r, c, canny_edge, mag_nms);
            cannyTraceEdge ( 1, -1, r, c, canny_edge, mag_nms);
        }
    }

    cv::Mat high_curvature_edge = canny_edge;

    cv::Mat occ_edge = cv::Mat(depth.size(), CV_8UC1, cv::Scalar(0));
    for(int r = 1; r<depth.rows-1; r++){
        for(int c = 1; c<depth.cols-1; c++){

            int dep_Dxy = depth.at<uint16_t>(r, c);
            if(dep_Dxy == 0) continue;

            float dx = 0;
            float dy = 0;
            int invalid_count = 0;
            // 8 neibor
            int dep_dn[3][3] = {{0}};
            bool invalid = false;
            for(int offset_r=-1; offset_r<=1; offset_r++){
                for(int offset_c=-1; offset_c<=1; offset_c++){
                    if(offset_c == 0 && offset_r == 0) continue;
                    int dep_nn = depth.at<uint16_t>(r+offset_r, c+offset_c);
                    if(dep_nn == 0){
                        invalid = true;
                        float factor = 1;
                        if(std::abs(offset_c) == 1 && std::abs(offset_r) == 1)
                            factor = 1/std::sqrt(2.0f);
                        dx += offset_c*factor;
                        dy += offset_r*factor;
                        invalid_count ++;
                    }else{
                        dep_dn[offset_r+1][offset_c+1] = dep_Dxy - dep_nn;
                    }
                }
            }
            if(!invalid){
                float max_d = 0;
                int max_offset_r = 0;
                int max_offset_c = 0;
                for(int i=0; i<3; i++){
                    for(int j=0; j<3; j++){

                        cv::Vec3f normal = normals.at<cv::Vec3f>(r+i, c+j);
                        cv::Vec3f vec = {
                            (j-1)/fx,
                            (i-1)/fy,
                            dep_dn[i][j]/1000.0f
                        };

                        float dist = std::abs(vec.dot(normal));

                        if(dist > max_d){
                            max_d = dist;
                            max_offset_r = i-1;
                            max_offset_c = j-1;
                        }
                    }
                }
                if(max_d > 0.05f){
                    occ_edge.at<uchar>(r+max_offset_r, c+max_offset_c) = 255;
                }
            }else{
//                occ_edge.at<uchar>(r, c) = 255;
                if(dx == 0 && dy == 0) continue;
                dx /= invalid_count; dy /= invalid_count;

                int corr_depth = 0;
                for(int radius = 1; radius < max_search_neighbors_; radius++){
                    int new_r = r + int(std::floor(dy*radius));
                    int new_c = c + int(std::floor(dx*radius));
                    if(new_r < 0 || new_r >= depth.rows ||
                       new_c < 0 || new_c > depth.cols) break;

                    int cur_depth = depth.at<uint16_t>(new_r, new_c);
                    if(cur_depth > 0){ corr_depth = cur_depth; break;}
                }

                if(corr_depth > 0){
                    int diff = corr_depth - dep_Dxy;
                    if(std::abs(diff) > 0.02 * dep_Dxy){
                        if(diff < 0){ // occluding edge, discard
//                            occ_edge.at<uchar>(r, c) = 255;
                        }else{
                            // occluding edge,
                            occ_edge.at<uchar>(r, c) = 255;

                        }
                    }
                }else{
                    // nan boundary
                }
            }
        }
    }
    cv::Mat dst;
    cv::bitwise_or(high_curvature_edge, occ_edge, dst);

//    cv::dilate(dst, dst, cv::Mat());

//    cv::bitwise_not(dst, dst);
//    cv::distanceTransform(dst, dst, CV_DIST_C, 3);  //dilute distance
    return dst;
}
