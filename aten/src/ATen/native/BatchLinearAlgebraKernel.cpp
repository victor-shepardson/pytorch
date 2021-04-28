#include <ATen/ATen.h>
#include <ATen/Dispatch.h>
#include <ATen/native/BatchLinearAlgebra.h>
#include <ATen/native/LinearAlgebraUtils.h>
#include <ATen/native/cpu/zmath.h>

#include <c10/util/irange.h>

#include <TH/TH.h>  // for USE_LAPACK

namespace at { namespace native {

namespace {

/*
Copies the lower (or upper) triangle of the square matrix to the other half and conjugates it.
This operation is performed in-place.
*/
template <typename scalar_t>
void apply_reflect_conj_tri_single(scalar_t* self, int64_t n, int64_t stride, bool upper) {
  std::function<void(int64_t, int64_t)> loop = [](int64_t, int64_t){};
  if (upper) {
    loop = [&](int64_t start, int64_t end) {
      for (int64_t i = start; i < end; i++) {
        for (int64_t j = i + 1; j < n; j++) {
          self[i * stride + j] = conj_impl(self[j * stride + i]);
        }
      }
    };
  } else {
    loop = [&](int64_t start, int64_t end) {
      for (int64_t i = start; i < end; i++) {
        for (int64_t j = 0; j < i; j++) {
          self[i * stride + j] = conj_impl(self[j * stride + i]);
        }
      }
    };
  }
  // For small matrices OpenMP overhead is too large
  if (n < 256) {
    loop(0, n);
  } else {
    at::parallel_for(0, n, 0, loop);
  }
}

/*
Computes the inverse of a symmetric (Hermitian) positive-definite matrix n-by-n matrix 'input' using the Cholesky factorization
This is an in-place routine, content of 'input' is overwritten.
'infos' is an int Tensor containing error codes for each matrix in the batched input.
For more information see LAPACK's documentation for POTRI routine.
*/
template <typename scalar_t>
void apply_cholesky_inverse(Tensor& input, Tensor& infos, bool upper) {
#ifndef USE_LAPACK
  TORCH_CHECK(false, "cholesky_inverse: LAPACK library not found in compilation");
#else
  char uplo = upper ? 'U' : 'L';

  auto input_data = input.data_ptr<scalar_t>();
  auto infos_data = infos.data_ptr<int>();
  auto input_matrix_stride = matrixStride(input);
  auto batch_size = batchCount(input);
  auto n = input.size(-2);
  auto lda = std::max<int64_t>(1, n);

  for (int64_t i = 0; i < batch_size; i++) {
    scalar_t* input_working_ptr = &input_data[i * input_matrix_stride];
    int* info_working_ptr = &infos_data[i];
    lapackCholeskyInverse<scalar_t>(uplo, n, input_working_ptr, lda, info_working_ptr);
    // LAPACK writes to only upper/lower part of the matrix leaving the other side unchanged
    apply_reflect_conj_tri_single<scalar_t>(input_working_ptr, n, lda, upper);
  }
#endif
}

// This is a type dispatching helper function for 'apply_cholesky_inverse'
Tensor& cholesky_inverse_kernel_impl(Tensor& result, Tensor& infos, bool upper) {
  // This function calculates the inverse matrix in-place
  // result should be in column major order and contain matrices to invert
  // the content of result is overwritten by 'apply_cholesky_inverse'
  AT_DISPATCH_FLOATING_AND_COMPLEX_TYPES(result.scalar_type(), "cholesky_inverse_out_cpu", [&]{
    apply_cholesky_inverse<scalar_t>(result, infos, upper);
  });
  return result;
}

template <typename scalar_t>
void apply_eig(const Tensor& self, bool eigenvectors, Tensor& vals_, Tensor& vecs_, int64_t* info_ptr) {
#ifndef USE_LAPACK
  TORCH_CHECK(false, "Calling torch.eig on a CPU tensor requires compiling ",
    "PyTorch with LAPACK. Please use PyTorch built with LAPACK support.");
#else
  using value_t = typename c10::scalar_value_type<scalar_t>::type;

  char jobvr = eigenvectors ? 'V' : 'N';
  int64_t n = self.size(-1);
  auto self_data = self.data_ptr<scalar_t>();

  auto vals_data = vals_.data_ptr<scalar_t>();
  scalar_t* wr = vals_data;

  scalar_t* vecs_data = eigenvectors ? vecs_.data_ptr<scalar_t>() : nullptr;
  int ldvr = eigenvectors ? n : 1;

  Tensor rwork;
  value_t* rwork_data = nullptr;
  if (self.is_complex()) {
    ScalarType real_dtype = toValueType(typeMetaToScalarType(self.dtype()));
    rwork = at::empty({n*2}, self.options().dtype(real_dtype));
    rwork_data = rwork.data_ptr<value_t>();
  }

  if (n > 0) {
    // call lapackEig once to get the optimal size for work data
    scalar_t wkopt;
    int info;
    lapackEig<scalar_t, value_t>('N', jobvr, n, self_data, n, wr,
      nullptr, 1, vecs_data, ldvr, &wkopt, -1, rwork_data, &info);
    int lwork = std::max<int>(1, real_impl<scalar_t, value_t>(wkopt));

    // call again to do the actual work
    Tensor work = at::empty({lwork}, self.dtype());
    lapackEig<scalar_t, value_t>('N', jobvr, n, self_data, n, wr,
      nullptr, 1, vecs_data, ldvr, work.data_ptr<scalar_t>(), lwork, rwork_data, &info);
    *info_ptr = info;
  }
#endif
}

std::tuple<Tensor, Tensor> eig_kernel_impl(const Tensor& self, bool& eigenvectors) {
  int64_t n = self.size(-1);
  // lapackEig function expects the input to be column major, or stride {1, n},
  // so we must set the stride manually since the default stride for tensors is
  // row major, {n, 1}
  Tensor self_ = at::empty_strided(
      {n, n},
      {1, n},
      at::TensorOptions(self.dtype()));
  self_.copy_(self);

  auto options = self.options().memory_format(LEGACY_CONTIGUOUS_MEMORY_FORMAT);

  // the API is slightly different for the complex vs real case: if the input
  // is complex, eigenvals will be a vector of complex. If the input is real,
  // eigenvals will be a (n, 2) matrix containing the real and imaginary parts
  // in each column
  Tensor vals_;
  if (self.is_complex()) {
      vals_ = at::empty({n}, options);
  } else {
      vals_ = at::empty_strided({n, 2}, {1, n}, options);
  }
  Tensor vecs_ = eigenvectors
                 ? at::empty_strided({n, n}, {1, n}, options)
                 : Tensor();

  int64_t info;
  AT_DISPATCH_FLOATING_AND_COMPLEX_TYPES(self.scalar_type(), "eig_cpu", [&]{
    apply_eig<scalar_t>(self_, eigenvectors, vals_, vecs_, &info);
  });
  singleCheckErrors(info, "eig_cpu");
  return std::tuple<Tensor, Tensor>(vals_, vecs_);
}

/*
  Computes the eigenvalues and eigenvectors of n-by-n matrix 'input'.
  This is an in-place routine, content of 'input', 'values', 'vectors' is overwritten.
  'infos' is an int Tensor containing error codes for each matrix in the batched input.
  For more information see LAPACK's documentation for GEEV routine.
*/
template <typename scalar_t>
void apply_linalg_eig(Tensor& values, Tensor& vectors, Tensor& input, Tensor& infos, bool compute_eigenvectors) {
#ifndef USE_LAPACK
  TORCH_CHECK(false, "Calling torch.linalg.eig on a CPU tensor requires compiling ",
    "PyTorch with LAPACK. Please use PyTorch built with LAPACK support.");
#else
  using value_t = typename c10::scalar_value_type<scalar_t>::type;

  char jobvr = compute_eigenvectors ? 'V' : 'N';
  char jobvl = 'N';  // only right eigenvectors are computed
  auto n = input.size(-1);
  auto lda = std::max<int64_t>(1, n);
  auto batch_size = batchCount(input);
  auto input_matrix_stride = matrixStride(input);
  auto values_stride = values.size(-1);
  auto input_data = input.data_ptr<scalar_t>();
  auto values_data = values.data_ptr<scalar_t>();
  auto infos_data = infos.data_ptr<int>();
  auto rvectors_data = compute_eigenvectors ? vectors.data_ptr<scalar_t>() : nullptr;
  scalar_t* lvectors_data = nullptr;  // only right eigenvectors are computed
  int64_t ldvr = compute_eigenvectors ? lda : 1;
  int64_t ldvl = 1;

  Tensor rwork;
  value_t* rwork_data = nullptr;
  if (input.is_complex()) {
    ScalarType real_dtype = toValueType(input.scalar_type());
    rwork = at::empty({lda * 2}, input.options().dtype(real_dtype));
    rwork_data = rwork.data_ptr<value_t>();
  }

  // call lapackEig once to get the optimal size for work data
  scalar_t work_query;
  lapackEig<scalar_t, value_t>(jobvl, jobvr, n, input_data, lda, values_data,
    lvectors_data, ldvl, rvectors_data, ldvr, &work_query, -1, rwork_data, &infos_data[0]);

  int lwork = std::max<int>(1, static_cast<int>(real_impl<scalar_t, value_t>(work_query)));
  Tensor work = at::empty({lwork}, input.dtype());
  auto work_data = work.data_ptr<scalar_t>();

  for (auto i = decltype(batch_size){0}; i < batch_size; i++) {
    scalar_t* input_working_ptr = &input_data[i * input_matrix_stride];
    scalar_t* values_working_ptr = &values_data[i * values_stride];
    scalar_t* rvectors_working_ptr = compute_eigenvectors ? &rvectors_data[i * input_matrix_stride] : nullptr;
    int* info_working_ptr = &infos_data[i];
    lapackEig<scalar_t, value_t>(jobvl, jobvr, n, input_working_ptr, lda, values_working_ptr,
      lvectors_data, ldvl, rvectors_working_ptr, ldvr, work_data, lwork, rwork_data, info_working_ptr);
  }
#endif
}

// This is a type dispatching helper function for 'apply_linalg_eig'
void linalg_eig_kernel(Tensor& eigenvalues, Tensor& eigenvectors, Tensor& infos, const Tensor& input, bool compute_eigenvectors) {
  // This function calculates the non-symmetric eigendecomposition in-place
  // tensors should be in batched column major memory format
  // the content of eigenvalues, eigenvectors and infos is overwritten by 'apply_linalg_eig'

  // apply_linalg_eig modifies in-place provided input matrix, therefore we need a copy
  Tensor input_working_copy = at::empty(input.transpose(-2, -1).sizes(), input.options());
  input_working_copy.transpose_(-2, -1);  // make input_working_copy to have Fortran contiguous memory layout
  input_working_copy.copy_(input);

  AT_DISPATCH_FLOATING_AND_COMPLEX_TYPES(input.scalar_type(), "linalg_eig_out_cpu", [&]{
    apply_linalg_eig<scalar_t>(eigenvalues, eigenvectors, input_working_copy, infos, compute_eigenvectors);
  });
}

/*
  Computes eigenvalues and eigenvectors of the input that is stored initially in 'vectors'.
  The computation is done in-place: 'vectors' stores the input and will be overwritten,
  'values' should be an allocated empty array.
  'infos' is used to store information for possible checks for error.
  'upper' controls the portion of input matrix to consider in computations
  'compute_eigenvectors' controls whether eigenvectors should be computed.
  This function doesn't do any error checks and it's assumed that every argument is valid.
*/
template <typename scalar_t>
void apply_lapack_eigh(Tensor& values, Tensor& vectors, Tensor& infos, bool upper, bool compute_eigenvectors) {
#ifndef USE_LAPACK
  TORCH_CHECK(
      false,
      "Calling torch.linalg.eigh or eigvalsh on a CPU tensor requires compiling ",
      "PyTorch with LAPACK. Please use PyTorch built with LAPACK support.");
#else
  using value_t = typename c10::scalar_value_type<scalar_t>::type;

  char uplo = upper ? 'U' : 'L';
  char jobz = compute_eigenvectors ? 'V' : 'N';

  auto n = vectors.size(-1);
  auto lda = std::max<int64_t>(1, n);
  auto batch_size = batchCount(vectors);

  auto vectors_stride = matrixStride(vectors);
  auto values_stride = values.size(-1);

  auto vectors_data = vectors.data_ptr<scalar_t>();
  auto values_data = values.data_ptr<value_t>();
  auto infos_data = infos.data_ptr<int>();

  // Using 'int' instead of int32_t or int64_t is consistent with the current LAPACK interface
  // It really should be changed in the future to something like lapack_int that depends on the specific LAPACK library that is linked
  // or switch to supporting only 64-bit indexing by default.
  int lwork = -1;
  int lrwork = -1;
  int liwork = -1;
  scalar_t lwork_query;
  value_t rwork_query;
  int iwork_query;

  // call lapackSyevd once to get the optimal size for work data
  scalar_t work_query;
  lapackSyevd<scalar_t, value_t>(jobz, uplo, n, vectors_data, lda, values_data,
    &lwork_query, lwork, &rwork_query, lrwork, &iwork_query, liwork, infos_data);

  lwork = std::max<int>(1, real_impl<scalar_t, value_t>(lwork_query));
  Tensor work = at::empty({lwork}, vectors.options());
  auto work_data = work.data_ptr<scalar_t>();

  liwork = std::max<int>(1, iwork_query);
  Tensor iwork = at::empty({liwork}, vectors.options().dtype(at::kInt));
  auto iwork_data = iwork.data_ptr<int>();

  Tensor rwork;
  value_t* rwork_data = nullptr;
  if (vectors.is_complex()) {
    lrwork = std::max<int>(1, rwork_query);
    rwork = at::empty({lrwork}, values.options());
    rwork_data = rwork.data_ptr<value_t>();
  }

  // Now call lapackSyevd for each matrix in the batched input
  for (const auto i : c10::irange(batch_size)) {
    scalar_t* vectors_working_ptr = &vectors_data[i * vectors_stride];
    value_t* values_working_ptr = &values_data[i * values_stride];
    int* info_working_ptr = &infos_data[i];
    lapackSyevd<scalar_t, value_t>(jobz, uplo, n, vectors_working_ptr, lda, values_working_ptr,
      work_data, lwork, rwork_data, lrwork, iwork_data, liwork, info_working_ptr);
    // The current behaviour for Linear Algebra functions to raise an error if something goes wrong
    // or input doesn't satisfy some requirement
    // therefore return early since further computations will be wasted anyway
    if (*info_working_ptr != 0) {
      return;
    }
  }
#endif
}

// This is a type dispatching helper function for 'apply_lapack_eigh'
void linalg_eigh_kernel(Tensor& eigenvalues, Tensor& eigenvectors, Tensor& infos, bool upper, bool compute_eigenvectors) {
  // This function calculates the symmetric/hermitian eigendecomposition
  // in-place tensors should be in batched column major memory format the
  // content of eigenvalues, eigenvectors and infos is overwritten by
  // 'apply_lapack_eigh'
  AT_DISPATCH_FLOATING_AND_COMPLEX_TYPES(
      eigenvectors.scalar_type(), "linalg_eigh_cpu", [&] {
        apply_lapack_eigh<scalar_t>(
            eigenvalues, eigenvectors, infos, upper, compute_eigenvectors);
      });
}

/*
  The geqrf function computes the QR decomposition of matrices stored in `input`.
  However, rather than producing a Q matrix directly, it produces a sequence of
  elementary reflectors which may later be composed to construct Q - for example
  with the orgqr or ormqr functions.

  Args:
  * `input` - [in] Input tensor for QR decomposition
              [out] QR decomposition result which contains:
              i)  The elements of R, on and above the diagonal.
              ii) Directions of the reflectors implicitly defining Q.
             Tensor with the directions of the elementary reflectors below the diagonal,
              it will be overwritten with the result
  * `tau` - [out] Tensor which will contain the magnitudes of the reflectors
            implicitly defining Q.
  * `m` - The number of rows of `input` to consider
  * `n` - The number of columns of `input` to consider (actual sizes of `input` could be larger)

  For further details, please see the LAPACK documentation for GEQRF.
*/
template <typename scalar_t>
static void apply_geqrf(const Tensor& input, const Tensor& tau, int64_t m, int64_t n) {
#ifndef USE_LAPACK
  TORCH_CHECK(
      false,
      "Calling torch.geqrf on a CPU tensor requires compiling ",
      "PyTorch with LAPACK. Please use PyTorch built with LAPACK support.");
#else
  using value_t = typename c10::scalar_value_type<scalar_t>::type;
  auto input_data = input.data_ptr<scalar_t>();
  auto tau_data = tau.data_ptr<scalar_t>();
  auto input_matrix_stride = matrixStride(input);
  auto tau_stride = tau.size(-1);
  auto batch_size = batchCount(input);
  auto lda = std::max<int>(1, m);

  int info;
  // Run once, first to get the optimum work size.
  // Since we deal with batches of matrices with the same dimensions, doing this outside
  // the loop saves (batch_size - 1) workspace queries which would provide the same result
  // and (batch_size - 1) calls to allocate and deallocate workspace using at::empty()
  int lwork = -1;
  scalar_t wkopt;
  lapackGeqrf<scalar_t>(m, n, input_data, lda, tau_data, &wkopt, lwork, &info);
  TORCH_INTERNAL_ASSERT_DEBUG_ONLY(info == 0);

  // if lwork is less than 'n' then a warning is printed:
  // Intel MKL ERROR: Parameter 7 was incorrect on entry to SGEQRF.
  lwork = std::max<int>(std::max<int>(1, n), real_impl<scalar_t, value_t>(wkopt));
  Tensor work = at::empty({lwork}, input.options());

  for (const auto i : c10::irange(batch_size)) {
    scalar_t* input_working_ptr = &input_data[i * input_matrix_stride];
    scalar_t* tau_working_ptr = &tau_data[i * tau_stride];

    // now compute the actual QR and tau
    lapackGeqrf<scalar_t>(m, n, input_working_ptr, lda, tau_working_ptr, work.data_ptr<scalar_t>(), lwork, &info);

    // info from lapackGeqrf only reports if the i-th parameter is wrong
    // so we don't need to check it all the time
    TORCH_INTERNAL_ASSERT_DEBUG_ONLY(info == 0);
  }
#endif
}

// This is a type dispatching helper function for 'apply_geqrf'
void geqrf_kernel(const Tensor& input, const Tensor& tau, int64_t m, int64_t n) {
  AT_DISPATCH_FLOATING_AND_COMPLEX_TYPES(input.scalar_type(), "geqrf_cpu", [&]{
    apply_geqrf<scalar_t>(input, tau, m, n);
  });
}

/*
  The orgqr function allows reconstruction of an orthogonal (or unitary) matrix Q,
  from a sequence of elementary reflectors, such as produced by the geqrf function.

  Args:
  * `self` - Tensor with the directions of the elementary reflectors below the diagonal,
              it will be overwritten with the result
  * `tau` - Tensor containing the magnitudes of the elementary reflectors
  * `n_columns` - The number of columns of Q to be computed

  For further details, please see the LAPACK documentation for ORGQR and UNGQR.
*/
template <typename scalar_t>
inline void apply_orgqr(Tensor& self, const Tensor& tau, int64_t n_columns) {
#ifndef USE_LAPACK
  TORCH_CHECK(false, "Calling torch.orgqr on a CPU tensor requires compiling ",
    "PyTorch with LAPACK. Please use PyTorch built with LAPACK support.");
#else
  // Some LAPACK implementations might not work well with empty matrices:
  // workspace query might return lwork as 0, which is not allowed (requirement is lwork >= 1)
  // We don't need to do any calculations in this case, so let's return early
  if (self.numel() == 0) {
    return;
  }

  using value_t = typename c10::scalar_value_type<scalar_t>::type;
  auto self_data = self.data_ptr<scalar_t>();
  auto tau_data = tau.data_ptr<scalar_t>();
  auto self_matrix_stride = matrixStride(self);
  auto tau_stride = tau.size(-1);
  auto batch_size = batchCount(self);
  auto m = self.size(-2);
  auto k = tau.size(-1);
  auto lda = std::max<int64_t>(1, m);
  int info;

  // LAPACK's requirement
  TORCH_INTERNAL_ASSERT(m >= n_columns);
  TORCH_INTERNAL_ASSERT(n_columns >= k);

  // Run once, first to get the optimum work size.
  // Since we deal with batches of matrices with the same dimensions, doing this outside
  // the loop saves (batch_size - 1) workspace queries which would provide the same result
  // and (batch_size - 1) calls to allocate and deallocate workspace using at::empty()
  int lwork = -1;
  scalar_t wkopt;
  lapackOrgqr<scalar_t>(m, n_columns, k, self_data, lda, tau_data, &wkopt, lwork, &info);
  TORCH_INTERNAL_ASSERT_DEBUG_ONLY(info == 0);
  lwork = std::max<int>(1, real_impl<scalar_t, value_t>(wkopt));
  Tensor work = at::empty({lwork}, self.options());

  for (int64_t i = 0; i < batch_size; i++) {
    scalar_t* self_working_ptr = &self_data[i * self_matrix_stride];
    scalar_t* tau_working_ptr = &tau_data[i * tau_stride];

    // now compute the actual Q
    lapackOrgqr<scalar_t>(m, n_columns, k, self_working_ptr, lda, tau_working_ptr, work.data_ptr<scalar_t>(), lwork, &info);

    // info from lapackOrgqr only reports if the i-th parameter is wrong
    // so we don't need to check it all the time
    TORCH_INTERNAL_ASSERT_DEBUG_ONLY(info == 0);
  }
#endif
}

// This is a type dispatching helper function for 'apply_orgqr'
Tensor& orgqr_kernel_impl(Tensor& result, const Tensor& tau, int64_t n_columns) {
  AT_DISPATCH_FLOATING_AND_COMPLEX_TYPES(result.scalar_type(), "orgqr_cpu", [&]{
    apply_orgqr<scalar_t>(result, tau, n_columns);
  });
  return result;
}

/*
Solves the matrix equation op(A) X = B
X and B are n-by-nrhs matrices, A is a unit, or non-unit, upper or lower triangular matrix
and op(A) is one of op(A) = A or op(A) = A^T or op(A) = A^H.
This is an in-place routine, content of 'B' is overwritten.
'upper' controls the portion of input matrix to consider in computations,
'transpose' if true then op(A) = A^T,
'unitriangular' if true then the diagonal elements of A are assumed to be 1
and the actual diagonal values are not used.
'infos' is an int Tensor containing error codes for each matrix in the batched input.
For more information see LAPACK's documentation for TRTRS routine.
*/
template<typename scalar_t>
void apply_triangular_solve(Tensor& A, Tensor& B, Tensor& infos, bool upper, bool transpose, bool conjugate_transpose, bool unitriangular) {
#ifndef USE_LAPACK
  TORCH_CHECK(
      false,
      "Calling torch.triangular_solve on a CPU tensor requires compiling ",
      "PyTorch with LAPACK. Please use PyTorch built with LAPACK support.");
#else
  char uplo = upper ? 'U' : 'L';
  char trans = transpose ? 'T' : 'N';
  trans = conjugate_transpose ? 'C' : trans;
  char diag = unitriangular ? 'U' : 'N';

  auto A_data = A.data_ptr<scalar_t>();
  auto B_data = B.data_ptr<scalar_t>();
  auto A_mat_stride = matrixStride(A);
  auto B_mat_stride = matrixStride(B);
  auto batch_size = batchCount(A);
  auto n = A.size(-2);
  auto nrhs = B.size(-1);
  auto lda = std::max<int64_t>(1, n);
  auto infos_data = infos.data_ptr<int>();

  for (const auto i : c10::irange(batch_size)) {
    scalar_t* A_working_ptr = &A_data[i * A_mat_stride];
    scalar_t* B_working_ptr = &B_data[i * B_mat_stride];
    int* info_working_ptr = &infos_data[i];
    lapackTriangularSolve<scalar_t>(uplo, trans, diag, n, nrhs, A_working_ptr, lda, B_working_ptr, lda, info_working_ptr);
    // The current behaviour for linear algebra functions to raise an error if something goes wrong
    // or input doesn't satisfy some requirement
    // therefore return early since further computations will be wasted anyway
    if (*info_working_ptr != 0) {
      return;
    }
  }
#endif
}

void triangular_solve_kernel(Tensor& A, Tensor& B, Tensor& infos, bool upper, bool transpose, bool conjugate_transpose, bool unitriangular) {
  AT_DISPATCH_FLOATING_AND_COMPLEX_TYPES(A.scalar_type(), "triangular_solve_cpu", [&]{
    apply_triangular_solve<scalar_t>(A, B, infos, upper, transpose, conjugate_transpose, unitriangular);
  });
}

} // anonymous namespace

REGISTER_ARCH_DISPATCH(cholesky_inverse_stub, DEFAULT, &cholesky_inverse_kernel_impl);
REGISTER_AVX_DISPATCH(cholesky_inverse_stub, &cholesky_inverse_kernel_impl);
REGISTER_AVX2_DISPATCH(cholesky_inverse_stub, &cholesky_inverse_kernel_impl);
REGISTER_VSX_DISPATCH(cholesky_inverse_stub, &cholesky_inverse_kernel_impl);

REGISTER_ARCH_DISPATCH(eig_stub, DEFAULT, &eig_kernel_impl);
REGISTER_AVX_DISPATCH(eig_stub, &eig_kernel_impl);
REGISTER_AVX2_DISPATCH(eig_stub, &eig_kernel_impl);
REGISTER_VSX_DISPATCH(eig_stub, &eig_kernel_impl);

REGISTER_ARCH_DISPATCH(linalg_eig_stub, DEFAULT, &linalg_eig_kernel);
REGISTER_AVX_DISPATCH(linalg_eig_stub, &linalg_eig_kernel);
REGISTER_AVX2_DISPATCH(linalg_eig_stub, &linalg_eig_kernel);
REGISTER_VSX_DISPATCH(linalg_eig_stub, &linalg_eig_kernel);

REGISTER_ARCH_DISPATCH(linalg_eigh_stub, DEFAULT, &linalg_eigh_kernel);
REGISTER_AVX_DISPATCH(linalg_eigh_stub, &linalg_eigh_kernel);
REGISTER_AVX2_DISPATCH(linalg_eigh_stub, &linalg_eigh_kernel);
REGISTER_VSX_DISPATCH(linalg_eigh_stub, &linalg_eigh_kernel);

REGISTER_ARCH_DISPATCH(geqrf_stub, DEFAULT, &geqrf_kernel);
REGISTER_AVX_DISPATCH(geqrf_stub, &geqrf_kernel);
REGISTER_AVX2_DISPATCH(geqrf_stub, &geqrf_kernel);
REGISTER_VSX_DISPATCH(geqrf_stub, &geqrf_kernel);

REGISTER_ARCH_DISPATCH(orgqr_stub, DEFAULT, &orgqr_kernel_impl);
REGISTER_AVX_DISPATCH(orgqr_stub, &orgqr_kernel_impl);
REGISTER_AVX2_DISPATCH(orgqr_stub, &orgqr_kernel_impl);
REGISTER_VSX_DISPATCH(orgqr_stub, &orgqr_kernel_impl);

REGISTER_ARCH_DISPATCH(triangular_solve_stub, DEFAULT, &triangular_solve_kernel);
REGISTER_AVX_DISPATCH(triangular_solve_stub, &triangular_solve_kernel);
REGISTER_AVX2_DISPATCH(triangular_solve_stub, &triangular_solve_kernel);
REGISTER_VSX_DISPATCH(triangular_solve_stub, &triangular_solve_kernel);

}} // namespace at::native
