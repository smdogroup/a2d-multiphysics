#ifndef A2D_SPARSE_AMG_H
#define A2D_SPARSE_AMG_H

#include <iomanip>
#include <iostream>
#include <memory>

#include "array.h"
#include "block_numeric.h"
#include "sparse_matrix.h"
#include "sparse_numeric.h"
#include "sparse_symbolic.h"
#include "utils/a2dprofiler.h"

namespace A2D {

/*
  Compute aggregates for a matrix A stored in CSR format

  The output consists of an array with all positive entries. Entries in the
  aggregation array outside the range [0, nrows) indicate an unaggregated
  variable.
*/
template <class I, class IdxArrayType>
I BSRMatStandardAggregation(const I nrows, const IdxArrayType& rowp,
                            const IdxArrayType& cols, std::vector<I>& aggr,
                            std::vector<I>& cpts) {
  const I not_aggregated = std::numeric_limits<I>::max();
  const I do_not_aggregate = std::numeric_limits<I>::max() - 1;

  // Initially fill the array with the not_aggregated tag
  std::fill(aggr.begin(), aggr.begin() + nrows, not_aggregated);
  I num_aggregates = 0;

  // First pass
  for (I i = 0; i < nrows; i++) {
    if (aggr[i] == not_aggregated) {
      // Determine whether all neighbors of this node are free (not already
      // aggregates)
      bool has_aggregated_neighbors = false;
      bool has_neighbors = false;

      const I jp_end = rowp[i + 1];
      for (I jp = rowp[i]; jp < jp_end; jp++) {
        const I j = cols[jp];
        if (i != j) {
          has_neighbors = true;
          if (aggr[j] != not_aggregated) {
            has_aggregated_neighbors = true;
            break;
          }
        }
      }

      if (!has_neighbors) {
        // isolated node, do not aggregate
        aggr[i] = do_not_aggregate;  // do not aggregate this node
      } else if (!has_aggregated_neighbors) {
        // Make an aggregate out of this node and its neighbors
        aggr[i] = num_aggregates;
        cpts[num_aggregates] = i;
        for (I jp = rowp[i]; jp < jp_end; jp++) {
          aggr[cols[jp]] = num_aggregates;
        }
        num_aggregates++;
      }
    }
  }

  // Second pass
  // Add unaggregated nodes to any neighboring aggregate
  for (I i = 0; i < nrows; i++) {
    if (aggr[i] == not_aggregated) {
      for (I jp = rowp[i]; jp < rowp[i + 1]; jp++) {
        const I j = cols[jp];

        if (aggr[j] != not_aggregated) {
          aggr[i] = aggr[j];
          break;
        }
      }
    }
  }

  // Third pass
  for (I i = 0; i < nrows; i++) {
    if (aggr[i] == not_aggregated) {
      // node i has not been aggregated
      aggr[i] = num_aggregates;
      cpts[num_aggregates] = i;

      const I jp_end = rowp[i + 1];
      for (I jp = rowp[i]; jp < jp_end; jp++) {
        const I j = cols[jp];

        // if (aggr[j] == 0) {  // unmarked neighbors
        if (aggr[j] == not_aggregated) {  // unmarked neighbors
          aggr[j] = num_aggregates;
        }
      }
      num_aggregates++;
    } else if (aggr[i] == do_not_aggregate) {
      aggr[i] = nrows + 1;
    }
  }

  return num_aggregates;
}

/*
  Compute the tentative prolongation operator P.

  This code works by taking the tentative prolongation operators

  At this point, P has the non-zero pattern stored, but does not yet contain
  the numerical values.

  B = P * R
  P^{T} * P = I

  The columns of P are orthonormal.
*/
template <typename I, typename T, index_t M, index_t N>
BSRMat<I, T, M, N>* BSRMatMakeTentativeProlongation(
    const I nrows, const I num_aggregates, const std::vector<I>& aggr,
    const MultiArrayNew<T* [M][N]>& B, MultiArrayNew<T* [N][N]>& R,
    double toler = 1e-10) {
  // Form the non-zero pattern for PT
  std::vector<I> rowp(num_aggregates + 1, 0);

  I nnz = 0;
  for (I i = 0; i < nrows; i++) {
    if (int(aggr[i]) >= 0 && aggr[i] < num_aggregates) {
      rowp[aggr[i] + 1]++;
      nnz++;
    }
  }

  for (I i = 0; i < num_aggregates; i++) {
    rowp[i + 1] += rowp[i];
  }

  std::vector<I> cols(nnz);
  for (I i = 0; i < nrows; i++) {
    if (int(aggr[i]) >= 0 && aggr[i] < num_aggregates) {
      cols[rowp[aggr[i]]] = i;
      rowp[aggr[i]]++;
    }
  }

  for (I i = num_aggregates; i > 0; i--) {
    rowp[i] = rowp[i - 1];
  }
  rowp[0] = 0;

  // Zero the entries in R
  A2D::BLAS::zero(R);

  // Create the transpose of the matrix
  BSRMat<I, T, N, M> PT(num_aggregates, nrows, nnz, rowp, cols);

  for (I i = 0; i < PT.nbrows; i++) {
    // Copy the block from the near null-space candidates into P
    const I jp_end = PT.rowp[i + 1];
    for (I jp = PT.rowp[i]; jp < jp_end; jp++) {
      I j = PT.cols[jp];
      for (I k1 = 0; k1 < N; k1++) {
        for (I k2 = 0; k2 < M; k2++) {
          PT.Avals(jp, k1, k2) = B(j, k2, k1);
        }
      }
    }

    // Use modified Gram-Schmidt to orthogonalize the rows of the matrix
    for (I k = 0; k < N; k++) {
      // Take the initial norm of the row
      T init_norm = 0.0;
      for (I jp = PT.rowp[i]; jp < jp_end; jp++) {
        for (I m = 0; m < M; m++) {
          init_norm += PT.Avals(jp, k, m) * PT.Avals(jp, k, m);
        }
      }
      init_norm = A2D::sqrt(init_norm);
      double theta = absfunc(init_norm);

      // Take the dot product between rows k and j
      for (I j = 0; j < k; j++) {
        T dot = 0.0;
        for (I jp = PT.rowp[i]; jp < jp_end; jp++) {
          for (I m = 0; m < M; m++) {
            dot += PT.Avals(jp, j, m) * PT.Avals(jp, k, m);
          }
        }

        // Record the result into the R basis
        R(i, j, k) = dot;

        // Subtract the dot product time row j from row k
        for (I jp = PT.rowp[i]; jp < jp_end; jp++) {
          for (I m = 0; m < M; m++) {
            PT.Avals(jp, k, m) -= dot * PT.Avals(jp, j, m);
          }
        }
      }

      // Take the norm of row k
      T norm = 0.0;
      for (I jp = PT.rowp[i]; jp < jp_end; jp++) {
        for (I m = 0; m < M; m++) {
          norm += PT.Avals(jp, k, m) * PT.Avals(jp, k, m);
        }
      }
      norm = A2D::sqrt(norm);

      // Compute the scalar factor for this row - zero rows
      // that are nearly linearly dependent
      T scale = 0.0;
      if (absfunc(norm) > toler * theta) {
        scale = 1.0 / norm;
        R(i, k, k) = norm;
      } else {
        std::cerr << "BSRMatMakeTentativeProlongation: Zeroed column"
                  << std::endl;
      }

      // Set the scale value
      for (I jp = PT.rowp[i]; jp < jp_end; jp++) {
        for (I m = 0; m < M; m++) {
          PT.Avals(jp, k, m) *= scale;
        }
      }
    }
  }

  return BSRMatMakeTranspose(PT);
}

/*
  Compute the Jacobi smoothing for the tentative prolongation operator P0

  P = (I - omega/rho(D^{-1} A) * rho(D^{-1} A ) * P0
*/
template <typename I, typename T, index_t M, index_t N>
BSRMat<I, T, M, N>* BSRJacobiProlongationSmoother(T omega,
                                                  BSRMat<I, T, M, M>& A,
                                                  BSRMat<I, T, M, M>& Dinv,
                                                  BSRMat<I, T, M, N>& P0,
                                                  T* rho_) {
  // Compute DinvA <- Dinv * A
  BSRMat<I, T, M, M>* DinvA = BSRMatDuplicate(A);
  for (I i = 0; i < A.nbrows; i++) {
    for (I jp = A.rowp[i]; jp < A.rowp[i + 1]; jp++) {
      blockGemmSlice<T, M, M, M>(Dinv.Avals, Dinv.rowp[i], A.Avals, jp,
                                 DinvA->Avals, jp);
    }
  }

  // Estimate the spectral radius using Gerhsgorin
  // T rho = BSRMatGershgorinSpectralEstimate(*DinvA);

  // Spectral estimate using Arnoldi
  T rho = BSRMatArnoldiSpectralRadius(*DinvA);

  if (rho_) {
    *rho_ = rho;
  }

  // Compute the scalar multiple for the matrix-multiplication
  T scale = omega / rho;

  // Compute the non-zero pattern of the smoothed prolongation operator
  BSRMat<I, T, M, N>* P = BSRMatMatMultAddSymbolic(P0, *DinvA, P0);

  // Copy values P0 -> P
  BSRMatCopy(P0, *P);

  // Compute (P0 - scale * Dinv * A * P0) -> P
  BSRMatMatMultAddScale(-scale, *DinvA, P0, *P);

  delete DinvA;

  return P;
}

/*
  Compute the strength of connection matrix with the given tolerance
*/
template <typename I, typename T, index_t M>
void BSRMatStrengthOfConnection(T epsilon, BSRMat<I, T, M, M>& A,
                                std::vector<I>& rowp, std::vector<I>& cols) {
  // Frobenius norm squared for each diagonal entry
  std::vector<T> d(A.nbrows);
  T epsilon4 = epsilon * epsilon * epsilon * epsilon;

  if (A.diag.is_allocated()) {
    for (I i = 0; i < A.nbrows; i++) {
      I jp = A.diag[i];

      d[i] = 0.0;
      for (I ii = 0; ii < M; ii++) {
        for (I jj = 0; jj < M; jj++) {
          d[i] += A.Avals(jp, ii, jj) * A.Avals(jp, ii, jj);
        }
      }
    }
  } else {
    for (I i = 0; i < A.nbrows; i++) {
      I jp = A.find_column_index(i, i);

      if (jp != BSRMat<I, T, M, M>::NO_INDEX) {
        d[i] = 0.0;
        for (I ii = 0; ii < M; ii++) {
          for (I jj = 0; jj < M; jj++) {
            d[i] += A.Avals(jp, ii, jj) * A.Avals(jp, ii, jj);
          }
        }
      }
    }
  }

  rowp[0] = 0;
  for (I i = 0, nnz = 0; i < A.nbrows; i++) {
    I jp_end = A.rowp[i + 1];
    for (I jp = A.rowp[i]; jp < jp_end; jp++) {
      I j = A.cols[jp];

      if (i == j) {
        cols[nnz] = j;
        nnz++;
      } else {
        // Compute the Frobenius norm of the entry
        T af = 0.0;
        for (I ii = 0; ii < M; ii++) {
          for (I jj = 0; jj < M; jj++) {
            af += A.Avals(jp, ii, jj) * A.Avals(jp, ii, jj);
          }
        }

        if (A2D::RealPart(af * af) >= A2D::RealPart(epsilon4 * d[i] * d[j])) {
          cols[nnz] = j;
          nnz++;
        }
      }
    }

    rowp[i + 1] = nnz;
  }
}

/*
  Given the matrix A and the near null space basis B compute the block diagonal
  inverse of the matrix A, the prolongation and restriction operators, the
  reduced matrix Ar and the new near null space basis.
*/
template <typename I, typename T, index_t M, index_t N>
void BSRMatSmoothedAmgLevel(T omega, T epsilon, BSRMat<I, T, M, M>& A,
                            MultiArrayNew<T* [M][N]>& B,
                            BSRMat<I, T, M, M>** Dinv, BSRMat<I, T, M, N>** P,
                            BSRMat<I, T, N, M>** PT, BSRMat<I, T, N, N>** Ar,
                            MultiArrayNew<T* [N][N]>& Br, T* rho_) {
  I num_aggregates = 0;
  std::vector<I> aggr(A.nbcols);
  std::vector<I> cpts(A.nbcols);

  if (absfunc(epsilon) != 0.0) {
    // Compute the strength of connection S
    std::vector<I> Srowp(A.nbrows + 1);
    std::vector<I> Scols(A.nnz);
    BSRMatStrengthOfConnection(epsilon, A, Srowp, Scols);

    // Compute the aggregation - based on the strength of connection
    num_aggregates =
        BSRMatStandardAggregation(A.nbrows, Srowp, Scols, aggr, cpts);
  } else {
    num_aggregates =
        BSRMatStandardAggregation(A.nbrows, A.rowp, A.cols, aggr, cpts);
  }

  // Based on the aggregates, form a tentative prolongation operator
  MultiArrayNew<T* [N][N]> Br_("Br_", num_aggregates);

  BSRMat<I, T, M, N>* P0 =
      BSRMatMakeTentativeProlongation(A.nbrows, num_aggregates, aggr, B, Br_);

  // Get the diagonal block D^{-1}
  bool inverse = true;
  BSRMat<I, T, M, M>* Dinv_ = BSRMatExtractBlockDiagonal(A, inverse);

  // Smooth the prolongation operator
  BSRMat<I, T, M, N>* P_ =
      BSRJacobiProlongationSmoother(omega, A, *Dinv_, *P0, rho_);

  // Make the transpose operator
  BSRMat<I, T, N, M>* PT_ = BSRMatMakeTranspose(*P_);

  // AP = A * P
  BSRMat<I, T, M, N>* AP = BSRMatMatMultSymbolic(A, *P_);
  BSRMatMatMult(A, *P_, *AP);

  // Ar = PT * AP = PT * A * P
  BSRMat<I, T, N, N>* Ar_ = BSRMatMatMultSymbolic(*PT_, *AP);
  BSRMatMatMult(*PT_, *AP, *Ar_);

  // Copy over the values
  *Dinv = Dinv_;
  *P = P_;
  *PT = PT_;
  *Ar = Ar_;
  Br = Br_;

  delete P0;
  delete AP;
}

template <typename I, typename T, index_t M, index_t N>
class BSRMatAmg {
 public:
  BSRMatAmg(int num_levels, T omega, T epsilon,
            std::shared_ptr<BSRMat<I, T, M, M>> A, MultiArrayNew<T* [M][N]> B,
            bool print_info = false)
      : level(-1),
        A(A),
        B(B),
        P(NULL),
        PT(NULL),
        omega(omega),
        epsilon(epsilon),
        rho(0.0),
        Dinv(NULL),
        Afact(NULL),
        x(NULL),
        b(NULL),
        r(NULL),
        next(NULL) {
    makeAmgLevels(0, num_levels, print_info);
  }
  ~BSRMatAmg() {
    if (P) {
      delete P;
    }
    if (PT) {
      delete PT;
    }
    if (Dinv) {
      delete Dinv;
    }
    if (Afact) {
      delete Afact;
    }
    if (x) {
      delete x;
    }
    if (b) {
      delete b;
    }
    if (r) {
      delete r;
    }
    if (next) {
      delete next;
    }
  }

  /*
    Apply multigrid repeatedly until convergence
  */
  bool mg(MultiArrayNew<T* [M]>& b0, MultiArrayNew<T* [M]>& xk, I monitor = 0,
          I max_iters = 500, double rtol = 1e-8, double atol = 1e-30) {
    Timer timer("BSRMatAmg::mg()");
    // R == the residual
    MultiArrayNew<T* [M]> R("R", b0.layout());

    bool solve_flag = false;
    A2D::BLAS::zero(xk);
    A2D::BLAS::copy(R, b0);
    T init_norm = A2D::BLAS::norm(R);

    if (monitor) {
      std::printf("MG |A * x - b|[  0]: %20.10e\n", A2D::fmt(init_norm));
    }

    for (I iter = 0; iter < max_iters; iter++) {
      applyMg(b0, xk);

      A2D::BLAS::copy(R, b0);
      BSRMatVecMultSub(*A, xk, R);
      T res_norm = A2D::BLAS::norm(R);

      if ((iter + 1) % monitor == 0) {
        std::printf("MG |A * x - b|[%3d]: %20.10e\n", iter + 1,
                    A2D::fmt(res_norm));
      }

      if (absfunc(res_norm) < atol ||
          absfunc(res_norm) < rtol * absfunc(init_norm)) {
        if (monitor && !((iter + 1) % monitor == 0)) {
          std::printf("MG |A * x - b|[%3d]: %20.10e\n", iter + 1,
                      A2D::fmt(res_norm));
        }
        solve_flag = true;
        break;
      }
    }
    return solve_flag;
  }

  /*
    Apply the preconditioned conjugate gradient method.

    This uses the variant of PCG from the paper "Inexact Preconditioned
    Conjugate Gradient Method with Inner-Outer Iteration" by Golub and Ye.
  */
  bool cg(const std::function<void(MultiArrayNew<T* [M]>&,
                                   MultiArrayNew<T* [M]>&)>& mat_vec,
          MultiArrayNew<T* [M]>& b0, MultiArrayNew<T* [M]>& xk, I monitor = 0,
          I max_iters = 500, double rtol = 1e-8, double atol = 1e-30,
          I iters_per_reset = 100) {
    Timer timer("BSRMatAmg::cg()");
    // R, Z and P and work are temporary vectors
    // R == the residual
    auto b0_layout = b0.layout();
    MultiArrayNew<T* [M]> R("R", b0_layout);
    MultiArrayNew<T* [M]> Z("Z", b0_layout);
    MultiArrayNew<T* [M]> P("P", b0_layout);
    MultiArrayNew<T* [M]> work("work", b0_layout);

    bool solve_flag = false;
    A2D::BLAS::zero(xk);
    A2D::BLAS::copy(R, b0);  // R = b0
    T init_norm = A2D::BLAS::norm(R);

    for (I reset = 0, iter = 0; iter < max_iters; reset++) {
      if (reset > 0) {
        A2D::BLAS::copy(R, b0);          // R = b0
        mat_vec(xk, work);               // work = A * xk
        A2D::BLAS::axpy(R, -1.0, work);  // R = b0 - A * xk
      }

      if (monitor && reset == 0) {
        std::printf("PCG |A * x - b|[%3d]: %20.10e\n", iter,
                    A2D::fmt(init_norm));
      }

      if (absfunc(init_norm) > atol) {
        // Apply the preconditioner Z = M^{-1} R
        applyFactor(R, Z);

        // Set P = Z
        A2D::BLAS::copy(P, Z);

        // Compute rz = (R, Z)
        T rz = A2D::BLAS::dot(R, Z);

        for (I i = 0; i < iters_per_reset && iter < max_iters; i++, iter++) {
          mat_vec(P, work);                        // work = A * P
          T alpha = rz / A2D::BLAS::dot(work, P);  // alpha = (R, Z)/(A * P, P)
          A2D::BLAS::axpy(xk, alpha, P);           // x = x + alpha * P
          A2D::BLAS::axpy(R, -alpha, work);        // R' = R - alpha * A * P

          T res_norm = A2D::BLAS::norm(R);

          if ((iter + 1) % monitor == 0) {
            std::printf("PCG |A * x - b|[%3d]: %20.10e\n", iter + 1,
                        A2D::fmt(res_norm));
          }

          if (absfunc(res_norm) < atol ||
              absfunc(res_norm) < rtol * absfunc(init_norm)) {
            if (monitor && !((iter + 1) % monitor == 0)) {
              std::printf("PCG |A * x - b|[%3d]: %20.10e\n", iter + 1,
                          A2D::fmt(res_norm));
            }

            solve_flag = true;
            break;
          }

          applyFactor(R, work);                  // work = Z' = M^{-1} * R
          T rz_new = A2D::BLAS::dot(R, work);    // rz_new = (R', Z')
          T rz_old = A2D::BLAS::dot(R, Z);       // rz_old = (R', Z)
          T beta = (rz_new - rz_old) / rz;       // beta = (R', Z' - Z)/(R, Z)
          A2D::BLAS::axpby(P, 1.0, beta, work);  // P' = Z' + beta * P
          A2D::BLAS::copy(Z, work);              // Z <- Z'
          rz = rz_new;                           // rz <- (R', Z')
        }
      }

      if (solve_flag) {
        break;
      }
    }
    return solve_flag;
  }

  /*
    Apply one cycle of multigrid with the right-hand-side b and the non-zero
    solution x.
  */
  void applyMg(MultiArrayNew<T* [M]>& b_, MultiArrayNew<T* [M]>& x_) {
    // Set temporary variables
    MultiArrayNew<T* [M]>* bt = b;
    MultiArrayNew<T* [M]>* xt = x;
    b = &b_;
    x = &x_;
    applyMg();
    b = bt;
    x = xt;
  }

  /*
    Apply the multigrid cycle as a preconditioner. This will overwrite
    whatever entries are in x.
  */
  void applyFactor(MultiArrayNew<T* [M]>& b_, MultiArrayNew<T* [M]>& x_) {
    // Set temporary variables
    MultiArrayNew<T* [M]>* bt = b;
    MultiArrayNew<T* [M]>* xt = x;
    b = &b_;
    x = &x_;
    bool zero_solution = true;
    applyMg(zero_solution);
    b = bt;
    x = xt;
  }

  /*
    Update the values of Galerkin projection at each level without
    re-computing the basis
  */
  void update() {
    if (Afact) {
      // Copy values to the matrix
      BSRMatCopy(*A, *Afact);

      // Perform the numerical factorization
      BSRMatFactor(*Afact);
    } else if (next) {
      delete Dinv;
      bool inverse = true;
      Dinv = BSRMatExtractBlockDiagonal(*A, inverse);

      // AP = A * P
      BSRMat<I, T, M, N>* AP = BSRMatMatMultSymbolic(*A, *P);
      BSRMatMatMult(*A, *P, *AP);

      // next->A = PT * AP = PT * A * P
      BSRMatMatMult(*PT, *AP, *next->A);
      delete AP;

      next->update();
    }
  }

  /*
    Test the accuracy of the Galerkin operator
  */
  void testGalerkin() {
    auto x0 = r->duplicate();
    auto y0 = r->duplicate();
    auto xr = next->r->duplicate();
    auto yr1 = next->r->duplicate();
    auto yr2 = next->r->duplicate();
    A2D::BLAS::random(xr);

    // Compute P^{T} * A * P * xr
    BSRMatVecMult(*P, *xr, *x0);
    BSRMatVecMult(*A, *x0, *y0);
    BSRMatVecMult(*PT, *y0, *yr1);

    // Compute Ar * xr
    BSRMatVecMult(*next->A, *xr, *yr2);

    // compute the error
    A2D::BLAS::axpy(*yr1, -1.0, *yr2);
    T error = A2D::BLAS::norm(*yr1, *yr1);
    T rel_err = A2D::BLAS::norm(*yr1, *yr1) / A2D::BLAS::norm(*yr2, *yr2);
    std::printf("Galerkin operator check\n");
    std::printf(
        "||Ar * xr - P^{T} * A * P * xr||: %20.10e,  rel. err: %20.10e\n",
        fmt(error), fmt(rel_err));

    delete x0;
    delete y0;
    delete xr;
    delete yr1;
    delete yr2;
  }

 private:
  // Private constructor for initializing the class
  BSRMatAmg(T omega, T epsilon, std::shared_ptr<BSRMat<I, T, M, M>> A,
            MultiArrayNew<T* [M][N]> B)
      : level(-1),
        A(A),
        B(B),
        P(NULL),
        PT(NULL),
        omega(omega),
        epsilon(epsilon),
        rho(0.0),
        Dinv(NULL),
        Afact(NULL),
        x(NULL),
        b(NULL),
        r(NULL),
        next(NULL) {}

  // Declare a friend since the template parameters may be different at
  // different levels
  template <typename UI, typename UT, index_t UM, index_t UN>
  friend class BSRMatAmg;

  // Make the different multigrid levels
  void makeAmgLevels(int _level, int num_levels, bool print_info) {
    // Set the multigrid level
    level = _level;

    if (level == num_levels - 1) {
      x = new MultiArrayNew<T* [M]>("x", A->nbrows);
      b = new MultiArrayNew<T* [M]>("b", A->nbrows);

      // Form the sparse factorization - if the matrix is large and sparse, use
      // AMD, otherwise don't bother re-ordering.
      if (A->nbrows >= 20 && A->nnz < 0.25 * A->nbrows * A->nbrows) {
        Afact = BSRMatAMDFactorSymbolic(*A);

      } else {
        Afact = BSRMatFactorSymbolic(*A);
      }

      // Copy values to the matrix
      BSRMatCopy(*A, *Afact);

      // Perform the numerical factorization
      BSRMatFactor(*Afact);

      if (print_info) {
        printf("%10d%15d%15d\n", level, A->nbrows, Afact->nnz);
      }
    } else {
      r = new MultiArrayNew<T* [M]>("r", A->nbrows);
      if (level > 0) {
        r = new MultiArrayNew<T* [M]>("r", A->nbrows);
        x = new MultiArrayNew<T* [M]>("x", A->nbrows);
        b = new MultiArrayNew<T* [M]>("b", A->nbrows);
      }

      // Multicolor order this level
      BSRMatMultiColorOrder(*A);

      // Multi-color the matrix
      BSRMat<I, T, N, N>* Ar;
      MultiArrayNew<T* [N][N]> Br;

      // Find the new level
      BSRMatSmoothedAmgLevel<I, T, M, N>(omega, epsilon, *A, B, &Dinv, &P, &PT,
                                         &Ar, Br, &rho);

      // Allocate the next level
      auto Anext = std::shared_ptr<BSRMat<I, T, N, N>>(Ar);
      auto Bnext = MultiArrayNew<T* [N][N]>(Br);
      next = new BSRMatAmg<I, T, N, N>(omega, epsilon, Anext, Bnext);

      if (print_info) {
        if (level == 0) {
          printf("%10s%15s%15s%15s%15s\n", "Level", "n(A)", "nnz(A)", "nnz(P)",
                 "rho");
        }
        printf("%10d%15d%15d%15d%15.5f\n", level, A->nbrows, A->nnz, P->nnz,
               A2D::fmt(rho));
      }

      next->makeAmgLevels(level + 1, num_levels, print_info);
    }
  }

  void applyMg(bool zero_solution = false) {
    if (Afact) {
      BSRMatApplyFactor(*Afact, *b, *x);
    } else {
      // Pre-smooth with either a zero or non-zero x
      if (zero_solution) {
        A2D::BLAS::zero(*x);
      }
      T omega0 = 1.0;
      BSRApplySSOR(*Dinv, *A, omega0, *b, *x);

      // Compute the residuals r = b - A * x
      A2D::BLAS::copy(*r, *b);
      BSRMatVecMultSub(*A, *x, *r);

      // Now zero the solution on all subsequent levels
      zero_solution = true;

      // Restrict the residual to the next lowest level
      BSRMatVecMult(*PT, *r, *next->b);

      // Apply multigrid on the next lowest level
      next->applyMg(zero_solution);

      // Interpolate up from the next lowest grid level
      BSRMatVecMultAdd(*P, *next->x, *x);

      // Post-smooth
      BSRApplySSOR(*Dinv, *A, omega0, *b, *x);
    }
  }

  // Integer for the level
  int level;

  // Data for the matrix
  std::shared_ptr<BSRMat<I, T, M, M>> A;

  // The near null-space candidates
  MultiArrayNew<T* [M][N]> B;

  // Data for the prolongation and restriction
  BSRMat<I, T, M, N>* P;
  BSRMat<I, T, N, M>* PT;

  T omega;    // Omega value for constructing the prolongation operator
  T epsilon;  // Strength of connection value
  T rho;      // Estimate of the spectral radius

  BSRMat<I, T, M, M>* Dinv;  // Block diagonal inverse

  // Data for the full factorization (on the lowest level only)
  BSRMat<I, T, M, M>* Afact;

  // Data for the solution
  MultiArrayNew<T* [M]>* x;
  MultiArrayNew<T* [M]>* b;
  MultiArrayNew<T* [M]>* r;

  BSRMatAmg<I, T, N, N>* next;
};

}  // namespace A2D

#endif  // A2D_SPARSE_AMG_H
