//
// Created by hr on 22-12-5.
//
#include "lidar_selection.h"
#include "utils.h"

namespace faster_lio {

    namespace lidar_selection {
        LidarSelector::LidarSelector(const int gridsize, SparseMap *sparsemap) : grid_size(gridsize),
                                                                                 sparse_map(sparsemap) {
            downSizeFilter.setLeafSize(0.2, 0.2, 0.2);
            G = Matrix<double, DIM_STATE, DIM_STATE>::Zero();
            H_T_H = Matrix<double, DIM_STATE, DIM_STATE>::Zero();
            Rli = M3D::Identity();
            Rci = M3D::Identity();
            Rcw = M3D::Identity();
            Jdphi_dR = M3D::Identity();
            Jdp_dt = M3D::Identity();
            Jdp_dR = M3D::Identity();
            Pli = V3D::Zero();
            Pci = V3D::Zero();
            Pcw = V3D::Zero();
            width = 800;
            height = 600;
        }

        LidarSelector::~LidarSelector() {
            delete sparse_map;
            delete sub_sparse_map;
            delete[] grid_num;
            delete[] map_index;
            delete[] map_value;
            delete[] align_flag;
            delete[] patch_cache;
            unordered_map<int, Warp *>().swap(Warp_map);
            unordered_map<VOXEL_KEY, float>().swap(sub_feat_map);
            unordered_map<VOXEL_KEY, VOXEL_POINTS *>().swap(feat_map);
        }

        void LidarSelector::set_extrinsic(const faster_lio::V3D &transl, const faster_lio::M3D &rot) {
            Pli = -rot.transpose() * transl;
            Rli = rot.transpose();
        }

        void LidarSelector::init() {
            sub_sparse_map = new SubSparseMap;
            Rci = sparse_map->Rcl * Rli;
            Pci = sparse_map->Rcl * Pli + sparse_map->Pcl;
            M3D Ric;
            V3D Pic;
            Jdphi_dR = Rci;
            Pic = -Rci.transpose() * Pci;
            M3D tmp;
            tmp << SKEW_SYM_MATRIX(Pic); // 0.0, -Pic[2], Pic[1], Pic[2], 0.0, -Pic[0], -Pic[1], Pic[0], 0.0
            Jdp_dR = -Rci * tmp;
            width = cam->width();
            height = cam->height();
            grid_n_width = static_cast<int>(width / grid_size);  // 848/40
            grid_n_height = static_cast<int>(height / grid_size); // 480/40
            length = grid_n_width * grid_n_height;
            fx = cam->errorMultiplier2();
            fy = cam->errorMultiplier() / (4. * fx);
            grid_num = new int[length];
            map_index = new int[length];
            map_value = new float[length];
            align_flag = new int[length];
            map_dist = (float *) malloc(sizeof(float) * length);
            memset(grid_num, TYPE_UNKNOWN, sizeof(int) * length);
            memset(map_index, 0, sizeof(int) * length);
            memset(map_value, 0, sizeof(float) * length);
            voxel_points_.reserve(length);
            add_voxel_points_.reserve(length);
            count_img = 0;
            patch_size_total = patch_size * patch_size;
            patch_size_half = static_cast<int>(patch_size / 2);
            patch_cache = new float[patch_size_total];
            stage_ = STAGE_FIRST_FRAME;
            pg_down.reset(new PointCloudType());
            Map_points.reset(new PointCloudType());
            Map_points_output.reset(new PointCloudType());
            weight_scale_ = 10;
            weight_function_.reset(new vk::robust_cost::HuberWeightFunction());
            // weight_function_.reset(new vk::robust_cost::TukeyWeightFunction());
            scale_estimator_.reset(new vk::robust_cost::UnitScaleEstimator());
            // scale_estimator_.reset(new vk::robust_cost::MADScaleEstimator());
        }

        void LidarSelector::reset_grid() {
            // length = grid_n_width * grid_n_height = (width / grid_size) * (height / grid_size)网格个数:
            // width:848, height:480 grid_size:30 length = 1792
            memset(grid_num, TYPE_UNKNOWN, sizeof(int) * length);
            memset(map_index, 0, sizeof(int) * length);
            fill_n(map_dist, length, 10000);
            std::vector<PointPtr>(length).swap(voxel_points_);
            std::vector<V3D>(length).swap(add_voxel_points_);
            voxel_points_.reserve(length);
            add_voxel_points_.reserve(length);
        }

        // 误差对点的导数
        void LidarSelector::dpi(V3D p, MD(2, 3) &J) {
            const double x = p[0];
            const double y = p[1];
            const double z_inv = 1. / p[2];
            const double z_inv_2 = z_inv * z_inv;
            J(0, 0) = fx * z_inv;
            J(0, 1) = 0.0;
            J(0, 2) = -fx * x * z_inv_2;
            J(1, 0) = 0.0;
            J(1, 1) = fy * z_inv;
            J(1, 2) = -fy * y * z_inv_2;
        }

        void LidarSelector::getpatch(cv::Mat img, V2D pc, float *patch_tmp, int level) {
            const float u_ref = pc[0];
            const float v_ref = pc[1];
            const int scale = (1 << level); // 2 ^ level
            const int u_ref_i = floorf(pc[0] / scale) * scale; // 向下取整
            const int v_ref_i = floorf(pc[1] / scale) * scale;
            const float subpix_u_ref = (u_ref - u_ref_i) / scale;
            const float subpix_v_ref = (v_ref - v_ref_i) / scale;
            const float w_ref_tl = (1.0 - subpix_u_ref) * (1.0 - subpix_v_ref);
            const float w_ref_tr = subpix_u_ref * (1.0 - subpix_v_ref);
            const float w_ref_bl = (1.0 - subpix_u_ref) * subpix_v_ref;
            const float w_ref_br = subpix_u_ref * subpix_v_ref;
            for (int x = 0; x < patch_size; x++) {
                uint8_t *img_ptr = (uint8_t *) img.data + (v_ref_i - patch_size_half * scale + x * scale) * width +
                                   (u_ref_i - patch_size_half * scale);
                for (int y = 0; y < patch_size; y++, img_ptr += scale) {
                    patch_tmp[patch_size_total * level + x * patch_size + y] =
                            w_ref_tl * img_ptr[0] + w_ref_tr * img_ptr[scale] + w_ref_bl * img_ptr[scale * width] +
                            w_ref_br * img_ptr[scale * width + scale]; // TODO:??
                }
            }
        }

        /* Add new points to feat_map */
        void LidarSelector::addSparseMap(cv::Mat img, PointCloudType::Ptr pg) {
            reset_grid();

            for (int i = 0; i < pg->size(); i++) {
                V3D pt(pg->points[i].x, pg->points[i].y, pg->points[i].z);
                V2D pc(new_frame_->w2c(pt));
                if (new_frame_->cam_->isInFrame(pc.cast<int>(), (patch_size_half + 1) * 8)) {
                    int index =
                            static_cast<int>(pc[0] / grid_size) * grid_n_height + static_cast<int>(pc[1] / grid_size);
                    float cur_value = vk::shiTomasiScore(img, pc[0], pc[1]);

                    if (cur_value > map_value[index]) {
                        map_value[index] = cur_value;
                        add_voxel_points_[index] = pt;
                        grid_num[index] = TYPE_POINTCLOUD;
                    }
                }
            }

            int add = 0;
            for (int i = 0; i < length; i++) {
                if (grid_num[i] == TYPE_POINTCLOUD) {
                    V3D pt = add_voxel_points_[i];
                    V2D pc(new_frame_->w2c(pt));
                    float *patch = new float[patch_size_total * 3];
                    getpatch(img, pc, patch, 0);
                    getpatch(img, pc, patch, 1);
                    getpatch(img, pc, patch, 2);
                    PointPtr pt_new(new Point(pt));
                    Vector3d f = cam->cam2world(pc);
                    FeaturePtr ftr_new(new Feature(patch, pc, f, new_frame_->T_f_w_, map_value[i], 0));
                    ftr_new->img = new_frame_->img_pyr_[0];
                    ftr_new->id_ = new_frame_->id_;

                    pt_new->addFrameRef(ftr_new);
                    pt_new->value = map_value[i];
                    AddPoint(pt_new); // 把pt_new加入feat_map中
                    add += 1;
                }
            }
            printf("pg.size: %d \n", pg->size());
            printf("[ VIO ]: Add %d 3D points.\n", add);
        }

        void LidarSelector::AddPoint(PointPtr pt_new) {
            V3D pt_w(pt_new->pos_[0], pt_new->pos_[1], pt_new->pos_[2]);
            double voxel_size = 0.5; // TODO: 体素分辨率，写死了，能否改？
            float loc_xyz[3];
            for (int j = 0; j < 3; j++) {
                loc_xyz[j] = pt_w[j] / voxel_size;
                if (loc_xyz[j] < 0) {
                    loc_xyz[j] -= 1.0;
                }
            }
            VOXEL_KEY position((int64_t) loc_xyz[0], (int64_t) loc_xyz[1], (int64_t) loc_xyz[2]);
            auto iter = feat_map.find(position);
            if (iter != feat_map.end()) {
                iter->second->voxel_points.push_back(pt_new);
                iter->second->count++;
            } else {
                VOXEL_POINTS *ot = new VOXEL_POINTS(0);
                ot->voxel_points.push_back(pt_new);
                feat_map[position] = ot;
            }
        }

        void LidarSelector::getWarpMatrixAffine(const vk::AbstractCamera &cam, const Eigen::Vector2d &px_ref,
                                                const Eigen::Vector3d &f_ref, const double depth_ref,
                                                const Sophus::SE3 &T_cur_ref,
                                                const int level_ref, // the corresponding pyrimid level of px_ref
                                                const int pyramid_level, const int halfpatch_size,
                                                Eigen::Matrix2d &A_cur_ref) {
            // Compute affine warp matrix A_ref_cur
            const Vector3d xyz_ref(f_ref * depth_ref);
            Vector3d xyz_du_ref(
                    cam.cam2world(px_ref + Vector2d(halfpatch_size, 0) * (1 << level_ref) * (1 << pyramid_level)));
            Vector3d xyz_dv_ref(
                    cam.cam2world(px_ref + Vector2d(0, halfpatch_size) * (1 << level_ref) * (1 << pyramid_level)));
            xyz_du_ref *= xyz_ref[2] / xyz_du_ref[2];
            xyz_dv_ref *= xyz_ref[2] / xyz_dv_ref[2];
            const Vector2d px_cur(cam.world2cam(T_cur_ref * (xyz_ref)));
            const Vector2d px_du(cam.world2cam(T_cur_ref * (xyz_du_ref)));
            const Vector2d px_dv(cam.world2cam(T_cur_ref * (xyz_dv_ref)));
            // 最终的仿射矩阵
            A_cur_ref.col(0) = (px_du - px_cur) / halfpatch_size;
            A_cur_ref.col(1) = (px_dv - px_cur) / halfpatch_size;
        }

        void LidarSelector::warpAffine(const Eigen::Matrix2d &A_cur_ref, const cv::Mat &img_ref,
                                       const Eigen::Vector2d &px_ref, const int level_ref, const int search_level,
                                       const int pyramid_level, const int halfpatch_size, float *patch) {
            const int patch_size = halfpatch_size * 2;
            const Matrix2f A_ref_cur = A_cur_ref.inverse().cast<float>();
            if (isnan(A_ref_cur(0, 0))) {
                printf("Affine warp is NaN, probably camera has no translation\n"); // TODO
                return;
            }
//   Perform the warp on a larger patch.
//   float* patch_ptr = patch;
//   const Vector2f px_ref_pyr = px_ref.cast<float>() / (1<<level_ref) / (1<<pyramid_level);
//   const Vector2f px_ref_pyr = px_ref.cast<float>() / (1<<level_ref);
            for (int y = 0; y < patch_size; ++y) {
                for (int x = 0; x < patch_size; ++x)//, ++patch_ptr)
                {
                    // P[patch_size_total*level + x*patch_size+y]
                    Vector2f px_patch(x - halfpatch_size, y - halfpatch_size);
                    px_patch *= (1 << search_level);
                    px_patch *= (1 << pyramid_level);
                    const Vector2f px(A_ref_cur * px_patch + px_ref.cast<float>()); // 得到仿射变化后的patch坐标
                    if (px[0] < 0 || px[1] < 0 || px[0] >= img_ref.cols - 1 || px[1] >= img_ref.rows - 1)
                        patch[patch_size_total * pyramid_level + y * patch_size + x] = 0; // pyramid_level == 0
                        // *patch_ptr = 0;
                    else
                        patch[patch_size_total * pyramid_level + y * patch_size + x] = (float) vk::interpolateMat_8u(
                                img_ref, px[0], px[1]);
                    // *patch_ptr = (uint8_t) vk::interpolateMat_8u(img_ref, px[0], px[1]);
                }
            }
        }

        double LidarSelector::NCC(float *ref_patch, float *cur_patch, int patch_size) {
            double sum_ref = std::accumulate(ref_patch, ref_patch + patch_size, 0.0);
            double mean_ref = sum_ref / patch_size;

            double sum_cur = std::accumulate(cur_patch, cur_patch + patch_size, 0.0);
            double mean_curr = sum_cur / patch_size;

            double numerator = 0, demoniator1 = 0, demoniator2 = 0;
            for (int i = 0; i < patch_size; i++) {
                double n = (ref_patch[i] - mean_ref) * (cur_patch[i] - mean_curr);
                numerator += n;
                demoniator1 += (ref_patch[i] - mean_ref) * (ref_patch[i] - mean_ref);
                demoniator2 += (cur_patch[i] - mean_curr) * (cur_patch[i] - mean_curr);
            }
            return numerator / sqrt(demoniator1 * demoniator2 + 1e-10);
        }

        int LidarSelector::getBestSearchLevel(const Eigen::Matrix2d &A_cur_ref, const int max_level) {
            // Compute patch level in other image
            int search_level = 0;
            double D = A_cur_ref.determinant(); // 行列式（几何意义） D是仿射变化后四边形的面积

            while (D > 3.0 && search_level < max_level) { // hr: make sure D <= 3 && search_level < 2
                search_level += 1; // TODO:why???
                D *= 0.25;
            }
            return search_level;
        }

        void LidarSelector::addFromSparseMap(cv::Mat img, PointCloudType::Ptr pg) {
            if (feat_map.size() <= 0) return;
            /* A. 初始化 */
            double ts0 = omp_get_wtime();

            pg_down->reserve(feat_map.size());
            downSizeFilter.setInputCloud(pg);
            downSizeFilter.filter(*pg_down);

            reset_grid();
            memset(map_value, 0, sizeof(float) * length);

            sub_sparse_map->reset(); // 存一些视觉子地图信息，如：align_errors;search_levels;errors;patch.......
            deque<PointPtr>().swap(sub_map_cur_frame_);

            float voxel_size = 0.5; // TODO: 体素分辨率，写死了，能否改？

            unordered_map<VOXEL_KEY, float>().swap(sub_feat_map);
            unordered_map<int, Warp *>().swap(Warp_map);

            // 单通道灰度值全零Mat
            cv::Mat depth_img = cv::Mat::zeros(height, width, CV_32FC1);
            float *it = (float *) depth_img.data; // TODO:12.6 换一种形式定义（等价）
//            float it[height * width] = {0.0};
            double t_insert, t_depth, t_position;
            t_insert = t_depth = t_position = 0;

            int loc_xyz[3];
            printf("A. initial depthmap: %.6lf \n", omp_get_wtime() - ts0);

            /* B. 投影 && 存深度信息 */
            double ts1 = omp_get_wtime();
            for (int i = 0; i < pg_down->size(); i++) {
                // Transform Point to world coordinate
                V3D pt_w(pg_down->points[i].x, pg_down->points[i].y, pg_down->points[i].z);

                // Determine the key of hash table
                for (int j = 0; j < 3; j++) {
                    loc_xyz[j] = floor(pt_w[j] / voxel_size); // voxel_size:0.5 floor:向下取整函数,取不超过x的最大整数
                }
                VOXEL_KEY position(loc_xyz[0], loc_xyz[1], loc_xyz[2]);

                auto iter = sub_feat_map.find(position);
                if (iter == sub_feat_map.end()) {
                    sub_feat_map[position] = 1.0;
                }

                V3D pt_c(new_frame_->w2f(pt_w));
                V2D px;
                if (pt_c[2] > 0) {
                    px[0] = fx * pt_c[0] / pt_c[2] + cx;
                    px[1] = fy * pt_c[1] / pt_c[2] + cy;

                    // TODO:boundary的确定
                    if (new_frame_->cam_->isInFrame(px.cast<int>(), (patch_size_half + 1) * 8)) {
                        float depth = pt_c[2];
                        int col = int(px[0]);
                        int row = int(px[1]);
                        it[width * row + col] = depth;
                    }
                }
            }
            if(depth_img_en_){
                imshow("depth_img", depth_img);
            }
            printf("B. projection: %.6lf \n", omp_get_wtime() - ts0);

            /* C.feat_map.find */
            double t1 = omp_get_wtime();
            // 遍历所有子特征地图，并在特征地图中进行查找
            for (auto &iter: sub_feat_map) {
                VOXEL_KEY position = iter.first;
                auto corre_voxel = feat_map.find(position);

                // 如果找到，遍历所有特征地图的体素点
                if (corre_voxel != feat_map.end()) {
                    vector<PointPtr> &voxel_points = corre_voxel->second->voxel_points;
                    int voxel_num = voxel_points.size();
                    for (int i = 0; i < voxel_num; i++) {
                        PointPtr pt = voxel_points[i];
                        if (pt == nullptr) continue;
                        V3D pt_cam(new_frame_->w2f(pt->pos_));
                        if (pt_cam[2] < 0) continue;

                        V2D pc(new_frame_->w2c(pt->pos_));

                        FeaturePtr ref_ftr;

                        // TODO:boundary的确定
                        if (new_frame_->cam_->isInFrame(pc.cast<int>(), (patch_size_half + 1) * 8)) {
                            // pc在网格中的索引值
                            int index = static_cast<int>(pc[0] / grid_size) * grid_n_height +
                                        static_cast<int>(pc[1] / grid_size);
                            grid_num[index] = TYPE_MAP; // 确定为地图点
                            Vector3d obs_vec(new_frame_->pos() - pt->pos_);
                            float cur_dist = obs_vec.norm();
                            float cur_value = pt->value;
                            if (cur_dist <= map_dist[index]) {
                                map_dist[index] = cur_dist;
                                voxel_points_[index] = pt;
                            }

                            if (cur_value >= map_value[index]) {
                                map_value[index] = cur_value;
                            }
                        }
                    }
                }
            }

            /* D. 根据error更新sub_sparse_map */
            double t_2, t_3, t_4, t_5;
            t_2 = t_3 = t_4 = t_5 = 0;
            // 遍历所有的网格
            for (int i = 0; i < length; i++) {
                if (grid_num[i] == TYPE_MAP) {
                    double t_1 = omp_get_wtime();
                    PointPtr pt = voxel_points_[i];
                    if (pt == nullptr) continue;

                    V2D pc(new_frame_->w2c(pt->pos_));
                    V3D pt_cam(new_frame_->w2f(pt->pos_));

                    // 判断点深度连续性 TODO:这里true表示不连续，false表示连续 12.7 depth_continous -> depth_discontinus
                    bool depth_discontinous = false;
                    // 遍历patch的每一行的每一列
                    for (int u = -patch_size_half; u <= patch_size_half; u++) {
                        for (int v = -patch_size_half; v <= patch_size_half; v++) {
                            if (u == 0 && v == 0) continue;
                            float depth = it[width * (v + int(pc[1])) + u + int(pc[0])];
                            if (depth == 0.) continue;

                            double delta_dist = abs(pt_cam[2] - depth);
                            if (delta_dist > 1.5) {
                                depth_discontinous = true;
                                break;
                            }
                        }
                        if (depth_discontinous) break;
                    }
                    if (depth_discontinous) continue;

                    FeaturePtr ref_ftr;
                    // get frame with same point of view AND same pyramid level
                    // 找到与当前图像的观察角度最接近的点作为参考 <=60度
                    if (!pt->getCloseViewObs(new_frame_->pos(), ref_ftr, pc)) continue; // 函数实现中并没有用到传入的pc

                    float *patch_wrap = new float[patch_size_total * 3];
                    patch_wrap = ref_ftr->patch;
                    t_1 = omp_get_wtime();

                    int search_level;
                    Matrix2d A_cur_ref_zero;
                    auto iter_warp = Warp_map.find(ref_ftr->id_);
                    if (iter_warp != Warp_map.end()) {
                        search_level = iter_warp->second->search_level;
                        A_cur_ref_zero = iter_warp->second->A_cur_ref;
                    } else {
                        getWarpMatrixAffine(*cam, ref_ftr->px, ref_ftr->f, (ref_ftr->pos() - pt->pos_).norm(),
                                            new_frame_->T_f_w_ * ref_ftr->T_f_w_.inverse(), 0, 0, patch_size_half,
                                            A_cur_ref_zero);

                        search_level = getBestSearchLevel(A_cur_ref_zero, 2);

                        Warp *ot = new Warp(search_level, A_cur_ref_zero);
                        Warp_map[ref_ftr->id_] = ot;
                    }

                    for (int pyramid_level = 0; pyramid_level <= 0; pyramid_level++) {
                        warpAffine(A_cur_ref_zero, ref_ftr->img, ref_ftr->px, ref_ftr->level, search_level,
                                   pyramid_level, patch_size_half, patch_wrap);
                    }

                    getpatch(img, pc, patch_cache, 0);

                    float error = 0.0;
                    for (int ind = 0; ind < patch_size_total; ind++) {
                        error += (patch_wrap[ind] - patch_cache[ind]) * (patch_wrap[ind] - patch_cache[ind]); // 光度误差
                    }
                    if (error > outlier_threshold * patch_size_total) continue; // TODO：阈值 筛选合适的光度误差

                    // 更新子地图
                    sub_map_cur_frame_.push_back(pt);

                    sub_sparse_map->align_errors.push_back(error);
                    sub_sparse_map->propa_errors.push_back(error);
                    sub_sparse_map->search_levels.push_back(search_level);
                    sub_sparse_map->errors.push_back(error);
                    sub_sparse_map->index.push_back(i);
                    sub_sparse_map->voxel_points.push_back(pt);
                    sub_sparse_map->patch.push_back(patch_wrap); // 此时存入的是（pt通过getCloseViewObs()找到参考帧之后）参考帧的patch
                }
            }
            printf("[ VIO ]: choose %d points from sub_sparse_map.\n", int(sub_sparse_map->index.size()));

        }

        float LidarSelector::UpdateState(cv::Mat img, float total_residual, int level) {
            int total_points = sub_sparse_map->index.size();
            if (total_points == 0) return 0;
            StatesGroup old_state = (*state);
            V2D pc;
            MD(1, 2) Jimg;
            MD(2, 3) Jdpi;
            MD(1, 3) Jdphi, Jdp, JdR, Jdt;
            VectorXd z;
            bool EKF_end = false;
            /* Compute J */
            float error = 0.0, last_error = total_residual, patch_error = 0.0, last_patch_error = 0.0, propa_error = 0.0;
            bool z_init = true;
            const int H_DIM = total_points * patch_size_total;
            // K.resize(H_DIM, H_DIM);
            z.resize(H_DIM);
            z.setZero();

            // H.resize(H_DIM, DIM_STATE);
            // H.setZero();
            H_sub.resize(H_DIM, 6);
            H_sub.setZero();

            for (int iteration = 0; iteration < NUM_MAX_ITERATIONS; iteration++) {
                double count_outlier = 0;

                error = 0.0;
                propa_error = 0.0;
                n_meas_ = 0;
                M3D Rwi(state->rot_end);
                V3D Pwi(state->pos_end);
                Rcw = Rci * Rwi.transpose();
                Pcw = -Rci * Rwi.transpose() * Pwi + Pci;
                Jdp_dt = Rci * Rwi.transpose();

                M3D p_hat;
                int i;
                for (i = 0; i < sub_sparse_map->index.size(); i++) {
                    patch_error = 0;
                    int search_level = sub_sparse_map->search_levels[i];
                    int pyramid_level = level + search_level;
                    const int scale = (1 << pyramid_level);

                    PointPtr pt = sub_sparse_map->voxel_points[i];
                    if (pt == nullptr) continue;

                    V3D pf = Rcw * pt->pos_ + Pcw;
                    pc = cam->world2cam(pf);
                    // if((level==2 && iteration==0) || (level==1 && iteration==0) || level==0)
                    {
                        dpi(pf, Jdpi); // use pf(x,y,z) to return MD(2, 3) Jdpi
                        p_hat << SKEW_SYM_MATRIX(pf);
                    }
                    const float u_ref = pc[0];
                    const float v_ref = pc[1];
                    const int u_ref_i = floorf(pc[0] / scale) * scale;
                    const int v_ref_i = floorf(pc[1] / scale) * scale;
                    const float subpix_u_ref = (u_ref - u_ref_i) / scale;
                    const float subpix_v_ref = (v_ref - v_ref_i) / scale;
                    const float w_ref_tl = (1.0 - subpix_u_ref) * (1.0 - subpix_v_ref);
                    const float w_ref_tr = subpix_u_ref * (1.0 - subpix_v_ref);
                    const float w_ref_bl = (1.0 - subpix_u_ref) * subpix_v_ref;
                    const float w_ref_br = subpix_u_ref * subpix_v_ref;

                    float *P = sub_sparse_map->patch[i]; // 拿到参考帧的patch，对应论文中的Q_i
                    for (int x = 0; x < patch_size; x++) {
                        uint8_t *img_ptr =
                                (uint8_t *) img.data + (v_ref_i + x * scale - patch_size_half * scale) * width +
                                u_ref_i - patch_size_half * scale;
                        for (int y = 0; y < patch_size; ++y, img_ptr += scale) {
                            float du = 0.5f * ((w_ref_tl * img_ptr[scale] + w_ref_tr * img_ptr[scale * 2] +
                                                w_ref_bl * img_ptr[scale * width + scale] +
                                                w_ref_br * img_ptr[scale * width + scale * 2])
                                               - (w_ref_tl * img_ptr[-scale] + w_ref_tr * img_ptr[0] +
                                                  w_ref_bl * img_ptr[scale * width - scale] +
                                                  w_ref_br * img_ptr[scale * width]));
                            float dv = 0.5f *
                                       ((w_ref_tl * img_ptr[scale * width] + w_ref_tr * img_ptr[scale + scale * width] +
                                         w_ref_bl * img_ptr[width * scale * 2] +
                                         w_ref_br * img_ptr[width * scale * 2 + scale])
                                        - (w_ref_tl * img_ptr[-scale * width] +
                                           w_ref_tr * img_ptr[-scale * width + scale] +
                                           w_ref_bl * img_ptr[0] + w_ref_br * img_ptr[scale]));
                            Jimg << du, dv;
                            Jimg = Jimg * (1.0 / scale);
                            Jdphi = Jimg * Jdpi * p_hat;
                            Jdp = -Jimg * Jdpi;
                            JdR = Jdphi * Jdphi_dR + Jdp * Jdp_dR;
                            Jdt = Jdp * Jdp_dt;
                            double res = w_ref_tl * img_ptr[0] + w_ref_tr * img_ptr[scale] +
                                         w_ref_bl * img_ptr[scale * width] +
                                         w_ref_br * img_ptr[scale * width + scale] -
                                         P[patch_size_total * level + x * patch_size + y]; // 光度误差
                            z(i * patch_size_total + x * patch_size + y) = res;
                            patch_error += res * res;
                            n_meas_++;
                            H_sub.block<1, 6>(i * patch_size_total + x * patch_size + y, 0) << JdR, Jdt;
                        }
                    }
                    sub_sparse_map->errors[i] = patch_error;
                    error += patch_error;
                }

                error = error / n_meas_;
                if (error <= last_error) {
                    old_state = (*state);
                    last_error = error;

                    auto &&H_sub_T = H_sub.transpose();
                    H_T_H.block<6, 6>(0, 0) = H_sub_T * H_sub;
                    MD(DIM_STATE, DIM_STATE) &&K_1 = (H_T_H +
                                                      (state->cov / img_point_cov).inverse()).inverse(); // TODO：视觉协方差
                    auto &&HTz = H_sub_T * z;
                    // K = K_1.block<DIM_STATE,6>(0,0) * H_sub_T;
                    auto vec = (*state_propagat) - (*state);
                    G.block<DIM_STATE, 6>(0, 0) = K_1.block<DIM_STATE, 6>(0, 0) * H_T_H.block<6, 6>(0, 0);
                    auto solution = -K_1.block<DIM_STATE, 6>(0, 0) * HTz + vec -
                                    G.block<DIM_STATE, 6>(0, 0) * vec.block<6, 1>(0, 0);
                    (*state) += solution;
                    auto &&rot_add = solution.block<3, 1>(0, 0);
                    auto &&t_add = solution.block<3, 1>(3, 0);

                    if ((rot_add.norm() * 57.3f < 0.001f) &&
                        (t_add.norm() * 100.0f < 0.001f)) { // TODO:EKF结束判断阈值(视觉约束阈值)
                        EKF_end = true;
                    }
                } else {
                    (*state) = old_state;
                    EKF_end = true;
                }

                if (iteration == NUM_MAX_ITERATIONS || EKF_end) {
                    break;
                }
            }
            return last_error;
        }

        void LidarSelector::updateFrameState(StatesGroup state) {
            M3D Rwi(state.rot_end);
            V3D Pwi(state.pos_end);
            Rcw = Rci * Rwi.transpose();
            Pcw = -Rci * Rwi.transpose() * Pwi + Pci;
            new_frame_->T_f_w_ = Sophus::SE3(Rcw, Pcw);
        }

        /* 判断子稀疏地图中的体素点是否满足距离和视角条件，若满足，则将其加入到特征点链表中 */
        void LidarSelector::addObservation(cv::Mat img) {
            int total_points = sub_sparse_map->index.size();
            if (total_points == 0) return;

            for (int i = 0; i < total_points; i++) {
                PointPtr pt = sub_sparse_map->voxel_points[i];
                if (pt == nullptr) continue;
                V2D pc(new_frame_->w2c(pt->pos_));
                Sophus::SE3 pose_cur = new_frame_->T_f_w_;
                bool add_flag = false;
                // TODO:测试if条件加上的效果
                // if (sub_sparse_map->errors[i]<= 100*patch_size_total && sub_sparse_map->errors[i]>0) //&& align_flag[i]==1)
                {
                    float *patch_temp = new float[patch_size_total * 3];
                    getpatch(img, pc, patch_temp, 0);
                    getpatch(img, pc, patch_temp, 1);
                    getpatch(img, pc, patch_temp, 2);

                    //TODO: condition: distance and view_angle
                    // Step 1: time
                    FeaturePtr last_feature = pt->obs_.back();
                    // TODO:源代码if条件注释
                    if (new_frame_->id_ >= last_feature->id_ + 20) add_flag = true;
                    // Step 2: delta_pose
                    Sophus::SE3 pose_ref = last_feature->T_f_w_;
                    Sophus::SE3 delta_pose = pose_ref * pose_cur.inverse();
                    double delta_p = delta_pose.translation().norm();
                    double delta_theta = (delta_pose.rotation_matrix().trace() > 3.0 - 1e-6) ? 0.0 : std::acos(
                            0.5 * (delta_pose.rotation_matrix().trace() - 1));
                    if (delta_p > 0.5 || delta_theta > 10) add_flag = true;
                    // Step 3: pixel distance
                    Vector2d last_px = last_feature->px;
                    double pixel_dist = (pc - last_px).norm();
                    if (pixel_dist > 40) add_flag = true;

                    // Maintain the size of 3D Point observation features.
                    if (pt->obs_.size() >= 20) {
                        FeaturePtr ref_ftr;
                        pt->getFurthestViewObs(new_frame_->pos(), ref_ftr);
                        pt->deleteFeatureRef(ref_ftr);
                    }
                    if (add_flag) {
                        pt->value = vk::shiTomasiScore(img, pc[0], pc[1]);
                        Vector3d f = cam->cam2world(pc);
                        FeaturePtr ftr_new(new Feature(patch_temp, pc, f, new_frame_->T_f_w_, pt->value,
                                                       sub_sparse_map->search_levels[i]));
                        ftr_new->img = new_frame_->img_pyr_[0];
                        ftr_new->id_ = new_frame_->id_;

                        pt->addFrameRef(ftr_new); // obs_.push_front(ftr_new);
                    }
                }
            }
        }

        void LidarSelector::ComputeJ(cv::Mat img) {
            int total_points = sub_sparse_map->index.size();
            if (total_points == 0) return;
            float error = 1e10;
            float now_error = error;

            for (int level = 2; level >= 0; level--) {  // hr: a coarse-to-fine manner 2->0:粗糙->精细
                now_error = UpdateState(img, error, level); // last_error
            }
            if (now_error < error) {
                state->cov -= G * state->cov;
            }
            updateFrameState(*state); // Update T_f_w_
        }

        void LidarSelector::display_keypatch(double time) {
            int total_points = sub_sparse_map->index.size();
            if (total_points == 0) return;
            for (int i = 0; i < total_points; i++) {
                PointPtr pt = sub_sparse_map->voxel_points[i];
                V2D pc(new_frame_->w2c(pt->pos_));
                cv::Point2f pf;
                pf = cv::Point2f(pc[0], pc[1]);
                if (sub_sparse_map->errors[i] < 8000) // 5.5
                    cv::circle(img_cp, pf, 4, cv::Scalar(0, 255, 0), -1, 8); // Green Sparse Align tracked
                else
                    cv::circle(img_cp, pf, 4, cv::Scalar(255, 0, 0), -1, 8); // Blue Sparse Align tracked
            }
            std::string text = std::to_string(int(1 / time)) + " HZ";
            cv::Point2f origin;
            origin.x = 20;
            origin.y = 20;
            // 待绘制的图像；待绘制的文字；文本框左下角；字体；字体大小；线条颜色；.....
            cv::putText(img_cp, text, origin, cv::FONT_HERSHEY_COMPLEX, 0.6, cv::Scalar(255, 255, 255), 1, 8, 0);
        }

        V3F LidarSelector::getpixel(cv::Mat img, V2D pc) {
            const float u_ref = pc[0];
            const float v_ref = pc[1];
            const int u_ref_i = floorf(pc[0]);
            const int v_ref_i = floorf(pc[1]);
            const float subpix_u_ref = (u_ref - u_ref_i);
            const float subpix_v_ref = (v_ref - v_ref_i);
            const float w_ref_tl = (1.0 - subpix_u_ref) * (1.0 - subpix_v_ref);
            const float w_ref_tr = subpix_u_ref * (1.0 - subpix_v_ref);
            const float w_ref_bl = (1.0 - subpix_u_ref) * subpix_v_ref;
            const float w_ref_br = subpix_u_ref * subpix_v_ref;
            uint8_t *img_ptr = (uint8_t *) img.data + ((v_ref_i) * width + (u_ref_i)) * 3;
            float B = w_ref_tl * img_ptr[0] + w_ref_tr * img_ptr[0 + 3] + w_ref_bl * img_ptr[width * 3] +
                      w_ref_br * img_ptr[width * 3 + 0 + 3];
            float G = w_ref_tl * img_ptr[1] + w_ref_tr * img_ptr[1 + 3] + w_ref_bl * img_ptr[1 + width * 3] +
                      w_ref_br * img_ptr[width * 3 + 1 + 3];
            float R = w_ref_tl * img_ptr[2] + w_ref_tr * img_ptr[2 + 3] + w_ref_bl * img_ptr[2 + width * 3] +
                      w_ref_br * img_ptr[width * 3 + 2 + 3];
            V3F pixel(B, G, R);
            return pixel;
        }

        void LidarSelector::detect(cv::Mat img, PointCloudType::Ptr pg) {
            if (width != img.cols || height != img.rows) {
                std::cout << "cols: " << img.cols << "rows:" << img.rows << std::endl;
                std::cout << "Resize the img scale !!!" << std::endl;
                double scale = 0.5;
                cv::resize(img, img, cv::Size(img.cols * scale, img.rows * scale), 0, 0, CV_INTER_LINEAR);
            }
            img_rgb = img.clone();
            img_cp = img.clone();
            cv::cvtColor(img, img, CV_BGR2GRAY);

            new_frame_.reset(new Frame(cam, img.clone()));
            updateFrameState(*state); // get transformation of world to camera ：T_f_w

            if (stage_ == STAGE_FIRST_FRAME && pg->size() > 10) {
                new_frame_->setKeyframe(); // hr: find feature points(5 points method) and Select this frame as keyframe.
                stage_ = STAGE_DEFAULT_FRAME;
            }

            double t1 = omp_get_wtime();
            Timer::Evaluate(
                    [&, this]() {
                        addFromSparseMap(img, pg);
                    },
                    "addFromSparseMap");

            Timer::Evaluate(
                    [&, this]() {
                        addSparseMap(img, pg);
                    },
                    "addSparseMap");

            computeH = ekf_time = 0.0;
            Timer::Evaluate(
                    [&, this]() {
                        ComputeJ(img);
                    },
                    "ComputeJ");

            Timer::Evaluate(
                    [&, this]() {
                        addObservation(img);
                    },
                    "addObservation");
            double t2 = omp_get_wtime();
            frame_cont++;
            ave_total = ave_total * (frame_cont - 1) / frame_cont + (t2 - t1) / frame_cont;

            // TODO:use Timer::Evaluate calculate the use-time,and LOG(INFO)

            display_keypatch(t2 - t1); // 绘制关键patch
        }
    }

}