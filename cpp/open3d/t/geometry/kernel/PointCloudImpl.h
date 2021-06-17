// ----------------------------------------------------------------------------
// -                        Open3D: www.open3d.org                            -
// ----------------------------------------------------------------------------
// The MIT License (MIT)
//
// Copyright (c) 2018 www.open3d.org
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
// IN THE SOFTWARE.
// ----------------------------------------------------------------------------

#include <atomic>
#include <vector>

#include "open3d/core/Dispatch.h"
#include "open3d/core/Dtype.h"
#include "open3d/core/MemoryManager.h"
#include "open3d/core/SizeVector.h"
#include "open3d/core/Tensor.h"
#include "open3d/t/geometry/Utility.h"
#include "open3d/t/geometry/kernel/GeometryIndexer.h"
#include "open3d/t/geometry/kernel/GeometryMacros.h"
#include "open3d/t/geometry/kernel/PointCloud.h"
#include "open3d/utility/Console.h"
#include "open3d/utility/Timer.h"

#define O3D_MIN(a, b) a < b ? a : b
#define O3D_MAX(a, b) a > b ? a : b

namespace open3d {
namespace t {
namespace geometry {
namespace kernel {
namespace pointcloud {

#if defined(__CUDACC__)
void UnprojectCUDA
#else
void UnprojectCPU
#endif
        (const core::Tensor& depth,
         utility::optional<std::reference_wrapper<const core::Tensor>>
                 image_colors,
         core::Tensor& points,
         utility::optional<std::reference_wrapper<core::Tensor>> colors,
         const core::Tensor& intrinsics,
         const core::Tensor& extrinsics,
         float depth_scale,
         float depth_max,
         int64_t stride) {

    const bool have_colors = image_colors.has_value();
    NDArrayIndexer depth_indexer(depth, 2);
    NDArrayIndexer image_colors_indexer;

    core::Tensor pose = t::geometry::InverseTransformation(extrinsics);
    TransformIndexer ti(intrinsics, pose, 1.0f);

    // Output
    int64_t rows_strided = depth_indexer.GetShape(0) / stride;
    int64_t cols_strided = depth_indexer.GetShape(1) / stride;

    points = core::Tensor({rows_strided * cols_strided, 3},
                          core::Dtype::Float32, depth.GetDevice());
    NDArrayIndexer point_indexer(points, 1);
    NDArrayIndexer colors_indexer;
    if (have_colors) {
        const auto& imcol = image_colors.value().get();
        image_colors_indexer = NDArrayIndexer{imcol, 2};
        colors.value().get() =
                core::Tensor({rows_strided * cols_strided, 3},
                             core::Dtype::Float32, imcol.GetDevice());
        colors_indexer = NDArrayIndexer(colors.value().get(), 1);
    }

    // Counter
#if defined(__CUDACC__)
    core::Tensor count(std::vector<int>{0}, {}, core::Dtype::Int32,
                       depth.GetDevice());
    int* count_ptr = count.GetDataPtr<int>();
#else
    std::atomic<int> count_atomic(0);
    std::atomic<int>* count_ptr = &count_atomic;
#endif

    int64_t n = rows_strided * cols_strided;
#if defined(__CUDACC__)
    core::kernel::CUDALauncher launcher;
#else
    core::kernel::CPULauncher launcher;
#endif

    DISPATCH_DTYPE_TO_TEMPLATE(depth.GetDtype(), [&]() {
        launcher.LaunchGeneralKernel(n, [=] OPEN3D_DEVICE(
                                                int64_t workload_idx) {
            int64_t y = (workload_idx / cols_strided) * stride;
            int64_t x = (workload_idx % cols_strided) * stride;

            float d = *depth_indexer.GetDataPtr<scalar_t>(x, y) / depth_scale;
            if (d > 0 && d < depth_max) {
                int idx = OPEN3D_ATOMIC_ADD(count_ptr, 1);

                float x_c = 0, y_c = 0, z_c = 0;
                ti.Unproject(static_cast<float>(x), static_cast<float>(y), d,
                             &x_c, &y_c, &z_c);

                float* vertex = point_indexer.GetDataPtr<float>(idx);
                ti.RigidTransform(x_c, y_c, z_c, vertex + 0, vertex + 1,
                                  vertex + 2);
                if (have_colors) {
                    float* pcd_pixel = colors_indexer.GetDataPtr<float>(idx);
                    float* image_pixel =
                            image_colors_indexer.GetDataPtr<float>(x, y);
                    *pcd_pixel = *image_pixel;
                    *(pcd_pixel + 1) = *(image_pixel + 1);
                    *(pcd_pixel + 2) = *(image_pixel + 2);
                }
            }
        });
    });
#if defined(__CUDACC__)
    int total_pts_count = count.Item<int>();
#else
    int total_pts_count = (*count_ptr).load();
#endif

#ifdef __CUDACC__
    OPEN3D_CUDA_CHECK(cudaDeviceSynchronize());
#endif
    points = points.Slice(0, 0, total_pts_count);
    if (have_colors) {
        colors.value().get() =
                colors.value().get().Slice(0, 0, total_pts_count);
    }
}

template <typename T>
OPEN3D_HOST_DEVICE void EstimatePointWiseCovariance(const T* points_ptr,
                                                    const int64_t* indices_ptr,
                                                    const int64_t indices_size,
                                                    T* covariance_ptr) {
    T cumulants[9] = {0};

    for (int64_t i = 0; i < indices_size; i++) {
        int64_t idx = indices_ptr[i];
        cumulants[0] += points_ptr[idx];
        cumulants[1] += points_ptr[idx + 1];
        cumulants[2] += points_ptr[idx + 2];
        cumulants[3] += points_ptr[idx] * points_ptr[idx];
        cumulants[4] += points_ptr[idx] * points_ptr[idx + 1];
        cumulants[5] += points_ptr[idx] * points_ptr[idx + 2];
        cumulants[6] += points_ptr[idx + 1] * points_ptr[idx + 1];
        cumulants[7] += points_ptr[idx + 1] * points_ptr[idx + 2];
        cumulants[8] += points_ptr[idx + 2] * points_ptr[idx + 2];
    }

    T num_indices = static_cast<T>(indices_size);
    cumulants[0] /= num_indices;
    cumulants[1] /= num_indices;
    cumulants[2] /= num_indices;
    cumulants[3] /= num_indices;
    cumulants[4] /= num_indices;
    cumulants[5] /= num_indices;
    cumulants[6] /= num_indices;
    cumulants[7] /= num_indices;
    cumulants[8] /= num_indices;

    covariance_ptr[0] = cumulants[3] - cumulants[0] * cumulants[0];
    covariance_ptr[1] = cumulants[6] - cumulants[1] * cumulants[1];
    covariance_ptr[2] = cumulants[8] - cumulants[2] * cumulants[2];
    covariance_ptr[3] = cumulants[4] - cumulants[0] * cumulants[1];
    covariance_ptr[4] = covariance_ptr[1];
    covariance_ptr[5] = cumulants[5] - cumulants[0] * cumulants[2];
    covariance_ptr[6] = covariance_ptr[2];
    covariance_ptr[7] = cumulants[7] - cumulants[1] * cumulants[2];
    covariance_ptr[8] = covariance_ptr[5];
}

template <typename scalar_t>
void OPEN3D_HOST_DEVICE ComputeEigenvector0(const scalar_t* A,
                                            const scalar_t eval0,
                                            scalar_t* eigen_vector0) {
    scalar_t row0[3] = {A[0] - eval0, A[1], A[2]};
    scalar_t row1[3] = {A[1], A[4] - eval0, A[5]};
    scalar_t row2[3] = {A[2], A[5], A[8] - eval0};

    scalar_t r0xr1[3] = {row0[1] * row1[2] - row0[2] * row1[1],
                         row0[2] * row1[0] - row0[0] * row1[2],
                         row0[0] * row1[1] - row0[1] * row1[0]};
    scalar_t r0xr2[3] = {row0[1] * row2[2] - row0[2] * row2[1],
                         row0[2] * row2[0] - row0[0] * row2[2],
                         row0[0] * row2[1] - row0[1] * row2[0]};
    scalar_t r1xr2[3] = {row1[1] * row2[2] - row1[2] * row2[1],
                         row1[2] * row2[0] - row1[0] * row2[2],
                         row1[0] * row2[1] - row1[1] * row2[0]};

    scalar_t d0 =
            r0xr1[0] * r0xr1[0] + r0xr1[1] * r0xr1[1] + r0xr1[2] * r0xr1[2];
    scalar_t d1 =
            r0xr2[0] * r0xr2[0] + r0xr2[1] * r0xr2[1] + r0xr2[2] * r0xr2[2];
    scalar_t d2 =
            r1xr2[0] * r1xr2[0] + r1xr2[1] * r1xr2[1] + r1xr2[2] * r1xr2[2];

    scalar_t dmax = d0;
    int imax = 0;
    if (d1 > dmax) {
        dmax = d1;
        imax = 1;
    }
    if (d2 > dmax) {
        imax = 2;
    }

    if (imax == 0) {
        scalar_t sqrt_d = sqrt(d0);
        eigen_vector0[0] = r0xr1[0] / sqrt_d;
        eigen_vector0[1] = r0xr1[1] / sqrt_d;
        eigen_vector0[2] = r0xr1[2] / sqrt_d;
        return;
    } else if (imax == 1) {
        scalar_t sqrt_d = sqrt(d1);
        eigen_vector0[0] = r0xr2[0] / sqrt_d;
        eigen_vector0[1] = r0xr2[1] / sqrt_d;
        eigen_vector0[2] = r0xr2[2] / sqrt_d;
        return;
    } else {
        scalar_t sqrt_d = sqrt(d2);
        eigen_vector0[0] = r1xr2[0] / sqrt_d;
        eigen_vector0[1] = r1xr2[1] / sqrt_d;
        eigen_vector0[2] = r1xr2[2] / sqrt_d;
        return;
    }
}

template <typename scalar_t>
void OPEN3D_HOST_DEVICE ComputeEigenvector1(const scalar_t* A,
                                            const scalar_t* evec0,
                                            const scalar_t eval1,
                                            scalar_t* eigen_vector1) {
    scalar_t U[3];
    if (abs(evec0[0]) > abs(evec0[1])) {
        scalar_t inv_length =
                1.0 / sqrt(evec0[0] * evec0[0] + evec0[2] * evec0[2]);
        U[0] = -evec0[2] * inv_length;
        U[1] = 0.0;
        U[2] = evec0[0] * inv_length;
    } else {
        scalar_t inv_length =
                1.0 / sqrt(evec0[1] * evec0[1] + evec0[2] * evec0[2]);
        U[0] = 0.0;
        U[1] = evec0[2] * inv_length;
        U[2] = -evec0[1] * inv_length;
    }
    scalar_t V[3] = {evec0[1] * U[2] - evec0[2] * U[1],
                     evec0[2] * U[0] - evec0[0] * U[2],
                     evec0[0] * U[1] - evec0[1] * U[0]};

    scalar_t AU[3] = {A[0] * U[0] + A[1] * U[1] + A[2] * U[2],
                      A[1] * U[0] + A[4] * U[1] + A[5] * U[2],
                      A[2] * U[0] + A[5] * U[1] + A[8] * U[2]};

    scalar_t AV[3] = {A[0] * V[0] + A[1] * V[1] + A[2] * V[2],
                      A[1] * V[0] + A[4] * V[1] + A[5] * V[2],
                      A[2] * V[0] + A[5] * V[1] + A[8] * V[2]};

    scalar_t m00 = U[0] * AU[0] + U[1] * AU[1] + U[2] * AU[2] - eval1;
    scalar_t m01 = U[0] * AV[0] + U[1] * AV[1] + U[2] * AV[2];
    scalar_t m11 = V[0] * AV[0] + V[1] * AV[1] + V[2] * AV[2] - eval1;

    scalar_t absM00 = abs(m00);
    scalar_t absM01 = abs(m01);
    scalar_t absM11 = abs(m11);
    scalar_t max_abs_comp;

    if (absM00 >= absM11) {
        max_abs_comp = O3D_MAX(absM00, absM01);
        if (max_abs_comp > 0) {
            if (absM00 >= absM01) {
                m01 /= m00;
                m00 = 1 / sqrt(1 + m01 * m01);
                m01 *= m00;
            } else {
                m00 /= m01;
                m01 = 1 / sqrt(1 + m00 * m00);
                m00 *= m01;
            }
            eigen_vector1[0] = m01 * U[0] - m00 * V[0];
            eigen_vector1[1] = m01 * U[1] - m00 * V[1];
            eigen_vector1[2] = m01 * U[2] - m00 * V[2];
            return;
        } else {
            eigen_vector1[0] = U[0];
            eigen_vector1[1] = U[1];
            eigen_vector1[2] = U[2];
            return;
        }
    } else {
        max_abs_comp = O3D_MAX(absM11, absM01);
        if (max_abs_comp > 0) {
            if (absM11 >= absM01) {
                m01 /= m11;
                m11 = 1 / sqrt(1 + m01 * m01);
                m01 *= m11;
            } else {
                m11 /= m01;
                m01 = 1 / sqrt(1 + m11 * m11);
                m11 *= m01;
            }
            eigen_vector1[0] = m11 * U[0] - m01 * V[0];
            eigen_vector1[1] = m11 * U[1] - m01 * V[1];
            eigen_vector1[2] = m11 * U[2] - m01 * V[2];
            return;
        } else {
            eigen_vector1[0] = U[0];
            eigen_vector1[1] = U[1];
            eigen_vector1[2] = U[2];
            return;
        }
    }
}

template <typename scalar_t>
void OPEN3D_HOST_DEVICE EstimatePointWiseNormalsWithFastEigen3x3(
        const scalar_t* covariance_ptr, scalar_t* normals_ptr) {
    // Based on:
    // https://www.geometrictools.com/Documentation/RobustEigenSymmetric3x3.pdf
    // which handles edge cases like points on a plane.

    scalar_t max_coeff = covariance_ptr[0];
    for (int i = 1; i < 9; i++) {
        if (max_coeff > covariance_ptr[i]) {
            max_coeff = covariance_ptr[i];
        }
    }

    if (max_coeff == 0) {
        normals_ptr[0] = 0.0;
        normals_ptr[1] = 0.0;
        normals_ptr[2] = 0.0;
        return;
    }

    scalar_t A[9] = {0};

    for (int i = 0; i < 9; i++) {
        A[i] = covariance_ptr[i] / max_coeff;
    }

    scalar_t norm = A[1] * A[1] + A[2] * A[2] + A[5] * A[5];

    if (norm > 0) {
        scalar_t eval[3];
        scalar_t evec0[3];
        scalar_t evec1[3];
        scalar_t evec2[3];

        scalar_t q = (A[0] + A[4] + A[8]) / 3.0;

        scalar_t b00 = A[0] - q;
        scalar_t b11 = A[4] - q;
        scalar_t b22 = A[8] - q;

        scalar_t p =
                sqrt((b00 * b00 + b11 * b11 + b22 * b22 + norm * 2.0) / 6.0);

        scalar_t c00 = b11 * b22 - A[5] * A[5];
        scalar_t c01 = A[1] * b22 - A[5] * A[2];
        scalar_t c02 = A[1] * A[5] - b11 * A[2];
        scalar_t det = (b00 * c00 - A[1] * c01 + A[2] * c02) / (p * p * p);

        scalar_t half_det = det * 0.5;
        half_det = O3D_MIN(O3D_MAX(half_det, -1.0), 1.0);

        scalar_t angle = acos(half_det) / 3.0;
        const scalar_t two_thrids_pi = 2.09439510239319549;

        scalar_t beta2 = cos(angle) * 2.0;
        scalar_t beta0 = cos(angle + two_thrids_pi) * 2.0;
        scalar_t beta1 = -(beta0 + beta2);

        eval[0] = q + p * beta0;
        eval[1] = q + p * beta1;
        eval[2] = q + p * beta2;

        if (half_det >= 0) {
            ComputeEigenvector0(&A, eval[2], &evec2);

            if (eval[2] < eval[0] && eval[2] < eval[1]) {
                normals_ptr[0] = evec2[0];
                normals_ptr[1] = evec2[1];
                normals_ptr[2] = evec2[2];
                return;
            }

            ComputeEigenvector1(&A, &evec2, eval[1], &evec1);

            if (eval[1] < eval[0] && eval[1] < eval[2]) {
                normals_ptr[0] = evec1[0];
                normals_ptr[1] = evec1[1];
                normals_ptr[2] = evec1[2];
                return;
            }

            normals_ptr[0] = evec1[1] * evec2[2] - evec1[2] * evec2[1];
            normals_ptr[1] = evec1[2] * evec2[0] - evec1[0] * evec2[2];
            normals_ptr[2] = evec1[0] * evec2[1] - evec1[1] * evec2[0];
            return;
        } else {
            ComputeEigenvector0(&A, eval[0], &evec0);

            if (eval[0] < eval[1] && eval[0] < eval[2]) {
                normals_ptr[0] = evec0[0];
                normals_ptr[1] = evec0[1];
                normals_ptr[2] = evec0[2];
                return;
            }

            ComputeEigenvector1(&A, &evec0, eval[1], &evec1);

            if (eval[1] < eval[0] && eval[1] < eval[2]) {
                normals_ptr[0] = evec1[0];
                normals_ptr[1] = evec1[1];
                normals_ptr[2] = evec1[2];
                return;
            }

            normals_ptr[0] = evec0[1] * evec1[2] - evec0[2] * evec1[1];
            normals_ptr[1] = evec0[2] * evec1[0] - evec0[0] * evec1[2];
            normals_ptr[2] = evec0[0] * evec1[1] - evec0[1] * evec1[0];
            return;
        }
    } else {
        if (covariance_ptr[0] < covariance_ptr[4] &&
            covariance_ptr[0] < covariance_ptr[8]) {
            normals_ptr[0] = 1.0;
            normals_ptr[1] = 0.0;
            normals_ptr[2] = 0.0;
            return;
        } else if (covariance_ptr[0] < covariance_ptr[4] &&
                   covariance_ptr[0] < covariance_ptr[8]) {
            normals_ptr[0] = 0.0;
            normals_ptr[1] = 1.0;
            normals_ptr[2] = 0.0;
            return;
        } else {
            normals_ptr[0] = 0.0;
            normals_ptr[1] = 0.0;
            normals_ptr[2] = 1.0;
            return;
        }
    }
}

template void EstimatePointWiseCovariance(const float* points_ptr,
                                          const int64_t* indices_ptr,
                                          const int64_t indices_size,
                                          float* covariance_ptr);

template void EstimatePointWiseCovariance(const double* points_ptr,
                                          const int64_t* indices_ptr,
                                          const int64_t indices_size,
                                          double* covariance_ptr);

}  // namespace pointcloud
}  // namespace kernel
}  // namespace geometry
}  // namespace t
}  // namespace open3d
