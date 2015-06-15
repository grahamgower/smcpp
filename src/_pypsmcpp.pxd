from libcpp.vector cimport vector

cdef extern from "common.h":
    cdef cppclass adouble:
        pass
    cdef cppclass Matrix[T]:
        pass
    cdef double toDouble(const adouble &)
    void init_eigen()
    void fill_jacobian(const adouble &, double*)
    void store_matrix(const Matrix[double] &, double*)

cdef extern from "matrix_interpolator.h":
    cdef cppclass MatrixInterpolator:
        MatrixInterpolator(int, vector[double], vector[double*])

cdef extern from "inference_manager.h":
    ctypedef vector[vector[double]] ParameterVector
    cdef cppclass InferenceManager:
        InferenceManager(const MatrixInterpolator&, const int, const int,
                const vector[int*], const vector[double], const double,
                const double, const int, const int, const int)
        void setParams_d(const ParameterVector)
        void setParams_ad(const ParameterVector)
        void Estep()
        vector[double] loglik(double)
        vector[adouble] Q(double)

cdef extern from "conditioned_sfs.h":
    void set_seed(long long)
    void cython_calculate_sfs(const vector[vector[double]] &params,
            int n, int num_samples, const MatrixInterpolator&,
            double tau1, double tau2, int numthreads, double theta, 
            double* outsfs)
    void cython_calculate_sfs_jac(const vector[vector[double]] &params,
            int n, int num_samples, const MatrixInterpolator&,
            double tau1, double tau2, int numthreads, double theta, 
            double* outsfs, double* outjac)

cdef extern from "transition.h":
    void cython_calculate_transition(const vector[vector[double]] &params,
            const vector[double] hidden_states, double rho, double* outtrans)
    void cython_calculate_transition_jac(const vector[vector[double]] &params,
            const vector[double] hidden_states, double rho, double* outtrans, double* outjac)

    
