#include <cstdio>
#include <cmath>
#include <curand.h>
#include <iostream>
#include <vector>
#include <type_traits>
#include <cassert>
#include "openacc.h"
#include "neural.hpp"
#include "ops.hpp"
#include "tensor.hpp"
#include <iomanip>

#if defined(_DEBUG_OP) && !defined(_OPENACC)
#define SAFEDATA
#endif

using namespace std;

class cuGenerator {

private:
    curandGenerator_t gen;
    int istat, device_type;
    
public:
    cuGenerator(int dvtype) {
        device_type = dvtype;
        
        if (device_type == Neural::device_type_gpu) {
            cout <<  "cuGenerator gpu" <<  endl;
            istat = curandCreateGenerator(&gen, CURAND_RNG_PSEUDO_DEFAULT);
            reportError();
        }
        else if (device_type == Neural::device_type_host) {
            cout <<  "cuGenerator host" <<  endl;
            istat = curandCreateGeneratorHost(&gen, CURAND_RNG_PSEUDO_DEFAULT);
            reportError();
        }
        else {
            throw(std::invalid_argument("Device type unknown"));
        }
    }
    
    cuGenerator() {}
    
    ~cuGenerator() {
        istat = curandDestroyGenerator(gen);
        reportError();
    }
    
    void reportError() {
        if (istat != CURAND_STATUS_SUCCESS) {
            throw(std::runtime_error("Generator error"));
        }
    }
    
    curandGenerator_t &get_gen() {
        return gen;
    }
};


template<class T>
void generateNormal(T *data,  int n,  T mean,  T stddev) {
    cuGenerator *gen;
    int device_type = Neural::get_device_type();
    bool is_present = Neural::is_present(data,  n*sizeof(T));
    
    T *adata = data;
    
    LOG("Generate normal | device type: " << device_type << " | present: " << is_present);
    
    if ( (device_type == Neural::device_type_gpu) && is_present ) {
        LOG("Getting device ptr");
        gen = new cuGenerator(Neural::device_type_gpu);
        adata = (T*)Neural::deviceptr(data);
    }
    else {
        gen = new cuGenerator(Neural::device_type_host);
    }
    
    if constexpr(std::is_same<T,  double>::value) {
        LOG("double generator");
        
        curandGenerateNormalDouble(gen->get_gen(), adata, n, mean, stddev);
    }
    else if constexpr(std::is_same<T,  float>::value) {
        LOG("float generator");
        curandGenerateNormal(gen->get_gen(), adata, n, mean, stddev);
    }
    else {
        throw(std::invalid_argument("Data type not supported"));
    }
    
    delete gen;
}


template<class T>
void acc_val_i(Tensor4D<T> *A) {
    int asize = A->size();
    
    T * a_data = A->data();
    #pragma acc parallel loop present(a_data[:asize])
    for(int i = 0; i < asize; i++) {
        a_data[i] = (i+1)*29;
    }
}

template<class T>
void acc_copy(const Tensor4D<T> &A, Tensor4D<T> *B) {
    assert(A.size() == B->size());
    
    int asize = A.size();
    
    const T* adata = A.data();
    T *bdata = B->data();
    
    #pragma acc parallel loop present(adata[:asize], bdata[:asize])
    for(int i = 0; i < asize; i++) {
        bdata[i] = adata[i];
    }
}

template<class T>
void acc_add(Tensor4D<T> *a, const Tensor4D<T> &b) {
    assert(a->size() == b.size());
    
    T* a_data = a->data();
    const T *b_data = b.data();
    int a_size = a->size();
    
    #pragma acc parallel loop present(a_data[:a_size],  b_data[:a_size])
    for (int i = 0; i < a_size; i++) {
        a_data[i] += b_data[i];
    }
}

template<class T>
void acc_val(Tensor4D<T> *A, T val) {
    int asize = A->size();
    
    T * a_data = A->data();
    #pragma acc parallel loop present(a_data[:asize])
    for(int i = 0; i < asize; i++) {
        a_data[i] = val;
    }
}

template<class T>
void acc_zeros(Tensor4D<T> *A) {
    acc_val(A, (T)0.0f);
}

template<class T>
void acc_mltp(Tensor4D<T> *A, T mltp) {
    int asize = A->size();
    
    T *a_data = A->data();
    #pragma acc parallel loop present(a_data[:asize])
    for(int i = 0; i < asize; i++) {
        a_data[i] *= mltp;
    }
}

template<class T>
void acc_accumulate(const Tensor4D<T> &a, Tensor4D<T> *b) {
    Shape4D a_shape = a.shape(), b_shape = b->shape();
    
    assert(a_shape[1]==b_shape[1]);
    
    const T* a_data = a.data();
    T* b_data = b->data();
    
    int B = a_shape[0], M = a_shape[1];
    
    #pragma acc parallel loop present(a_data[:B*M], b_data[:1*M])
    for(int j = 0; j < M; j++) {
        double accm = 0.0f;
        #pragma acc loop reduction(+:accm)
        for(int i = 0; i < B; i++) {
            accm+=a_data[i*M + j];
        }
        b_data[j] = accm;
    }
}

template<class T>
void acc_rng(Tensor4D<T> *output, T mtlp) {
    //bool acc = output->is_present_gpu();
    T *a_data = output->data();
    T *b_data = a_data;
    int n = output->size();
    int pn = n;
    bool resized{false};

    LOG("acc_set_rngv n: " << n);;
    
    if(n%2 == 1) {
        pn = n + 1;
        b_data = new T[pn];
        resized=true;
    }
    
    T stddev = 1.0f, mean = 0.0f;
    
    T mx, mn, range, ml;
    
    #pragma acc data present(a_data[:n]) create(b_data[:pn])
    {
    // Tensor4D<T> * dptr = (Tensor4D<T> *)acc_deviceptr(a_data);
    // Create Normal distribution
    
    generateNormal<T>(b_data, pn, mean, stddev);
    
    if(resized) {
        //copy back to a_data
        #pragma acc parallel loop
        for(int i = 0; i < n; i++) {
            a_data[i] = b_data[i];
        }
    }
    
    //find max, min
    mx = 0;
    mn = a_data[0];
    
    #pragma acc parallel loop reduction(max:mx) reduction(min:mn)
    for(int i = 0; i < n; i++) {
        if(a_data[i] > mx) {
            mx = a_data[i];
        }
        
        if(a_data[i] < mn) {
            mn = a_data[i];
        }
    }
    
    //normalize
    range = mx - mn;
    ml = 2.0f * mtlp / range;
    #pragma acc parallel loop
    for (int i = 0; i < n; i++) {
        a_data[i] *=  ml;
    }
    
    }
    
    if(resized) {
        delete[] b_data;
    }
}

template<class T>
void acc_flip_spatial(Tensor4D<T> *input) {
    Shape4D shape = input->shape();
    
    int A = shape[0], B = shape[1], C = shape[2], D = shape[3];
    T *in_data = input->data();
    
    #pragma acc parallel loop collapse(4) present(in_data[:A*B*C*D])
    for(int i = 0; i < A; i++) {
        for(int j = 0; j < B; j++) {
            for(int k = 0; k < (C/2); k++) {
                for(int l = 0; l < (D/2); l++) {
                    T tmp = in_data[(i*B + j)*C*D + k*D + l];
                    
                    #ifdef SAFEDATA
                    printf("in_data[%d, %d, %d, %d] %+11.5f <-> in_data[%d, %d, %d, %d] %+11.5f\n", i, j, k, l, tmp, i, j, C-k-1, D-l-1, in_data[ (i*B + j)*C*D + (C-k-1)*D + (D-l-1)]);
                    #endif
                    
                    in_data[ (i*B + j)*C*D +     k*D +    l ] = in_data[ (i*B + j)*C*D + (C-k-1)*D + (D-l-1)];
                    in_data[ (i*B + j)*C*D + (C-k-1)*D + (D-l-1)] = tmp;
                }
            }
        }
    }
}

template <class T>
void acc_matrix_multiply(const Tensor4D<T> &A, const Tensor4D<T> &B, Tensor4D<T> *C) {
    Shape4D a_shape = A.shape(), b_shape = B.shape(), c_shape = C->shape();
    Shape4D a_shape_flat = a_shape.flat(1);
    
    if(b_shape.size() != (b_shape[0]*b_shape[1])) {
        throw(std::invalid_argument("Error: B is not MxKx1. "));
    }
    
    assert(a_shape_flat[1] == b_shape[0]);
    
    assert(a_shape_flat[0] == c_shape[0]);
    
    assert(b_shape[1] == c_shape[1]);
 
    
    int N = a_shape[0], K = b_shape[0], M = b_shape[1];
    const T *a_data = A.data(), *b_data = B.data();
    T *c_data = C->data();
    
    #pragma acc data present(a_data[:(N*K)], b_data[0:K*M], c_data[0:N*M])
    {

    #pragma acc parallel loop collapse(2)
    for(int i = 0; i < N; i++) {
        for(int j = 0; j < M; j++) {
            T csumd = 0.0f;
            #ifdef SAFEDATA
                printf("Out [%d, %d] = ", i, j);
            #endif
                
            #pragma acc loop seq reduction(+:csumd)
            for(int t = 0; t < K; t++) {
                #ifdef SAFEDATA
                    cout << " + [" << a_data[i*K + t] << " x " << b_data[t*M + j] << "]";
                #endif
                csumd += a_data[i*K + t] * b_data[t*M + j];
            }
            #ifdef SAFEDATA
                cout << " = " << csumd << endl;
            #endif
            c_data[i*M + j] = csumd;
        }
    }

    }
}


//TODO stride 2D?
template <class T>
void acc_convolution2D(const Tensor4D<T> &input, const Tensor4D<T> &filters, Tensor4D<T> *output, const vector<int> &stride) { 
    Shape4D in_shape = input.shape(), filter_shape = filters.shape(), out_shape = output->shape();

    int batch = in_shape[0];
    int in_channels = in_shape[1];
    int in_rows = in_shape[2];
    int in_cols = in_shape[3];
    
    int filter_height = filter_shape[2], filter_width = filter_shape[3];
    int stride_r = stride[0], stride_c = stride[1];
    int out_channels = filter_shape[0];
    
    int out_cols = out_shape[3];
    int out_rows = out_shape[2];
    
    if(in_channels != filter_shape[1]) {
        throw(std::invalid_argument("Error: input channels != filter_shape[1]"));
    }
    
    if(out_channels != out_shape[1]) {
        throw(std::invalid_argument("Error: output channels != output_shape[1]"));
    }
    
    if(batch != out_shape[0]) {
        throw(std::invalid_argument("Error: batch != output_shape[0]"));
    }
    
    const T *in_data = input.data(), *filter_data = filters.data();
    T* out_data = output->data();
    
    #pragma acc data present(in_data[:(batch*in_cols*in_rows*in_channels)]) \
    present(filter_data[:in_channels*out_channels*filter_height*filter_width]) \
    present(out_data[:(batch* out_channels * out_cols * out_rows)])
    {
        
    #pragma acc parallel loop collapse(4)
    for(int i = 0 ; i < batch; i++) {
        for(int och = 0; och < out_channels; och++) {
            for(int oh = 0; oh < out_rows; oh++) {
                for(int ow = 0; ow < out_cols; ow++) {
                    T bdhwsum = 0.0f;
                    #ifdef SAFEDATA
                    printf("Out[%d, %d, %d, %d] = ", i, och, oh, ow);
                    #endif
                    
                    #pragma acc loop seq collapse(3) reduction(+:bdhwsum)
                    for(int ich = 0; ich < in_channels; ich++) {
    //                         double csum = 0.0f;
                        for(int fi = 0; fi < filter_height; fi++) {
                            for(int fj = 0; fj < filter_width; fj++) {
                                #ifdef SAFEDATA
                                cout << " + [" << in_data[ (i*in_channels + ich)*in_rows*in_cols + (oh*stride_r + fi)*in_cols + ow*stride_c + fj ] << " x " << filter_data[ (och*in_channels + ich)*filter_height*filter_width + fi*filter_width + fj ] << "]";
                                #endif
                                
                                bdhwsum += in_data[ (i*in_channels + ich)*in_rows*in_cols + (oh*stride_r + fi)*in_cols + ow*stride_c + fj ] * filter_data[ (och*in_channels + ich)*filter_height*filter_width + fi*filter_width + fj ];
                            }
                        }
    //                         sum += csum;
                    }
                    
                    out_data[(i*out_channels + och)*out_cols*out_rows + oh*out_cols + ow] = bdhwsum;
                    #ifdef SAFEDATA
                    printf(" = %+011.5f\n", bdhwsum);
                    #endif
                }
            }
        }
    }
    
    }
    
}

void tparallel_conv5(double *conv_input, double *conv_filters, double *conv_output, int batch_size, int in_channels, int in_height, int in_width, int out_channels , int out_height, int out_width, int filter_size, int stride, bool debug) { 
    
    
    #pragma acc data pcopyin(conv_input[:(batch_size*in_width*in_height*in_channels)], conv_filters[:in_channels*filter_size*filter_size*out_channels]) pcopyout(conv_output[:(batch_size * out_width * out_height * out_channels)])
    {
        
    #pragma acc parallel loop collapse(4)
    for(int i = 0; i < batch_size; i++) {
        for(int d = 0; d < out_channels; d++) {
            for(int oh = 0; oh < out_height; oh++) {
                for(int ow = 0; ow < out_width; ow++) {
                    double bdhwsum = 0.0f;
                    
                    #pragma acc loop seq collapse(3) reduction(+:bdhwsum)
                    for(int ch = 0; ch < in_channels; ch++) {
//                         double csum = 0.0f;
                        for(int di = 0; di < filter_size; di++) {
                            for(int dj = 0; dj < filter_size; dj++) {
                                bdhwsum += conv_input[ (i*in_channels + ch)*in_height*in_width + (oh*stride + di)*in_width + ow*stride + dj ] * conv_filters[ (d*in_channels + ch)*filter_size*filter_size + di*filter_size + dj ];
                            }
                        }
//                         sum += csum;
                    }
                    
                    conv_output[(i*out_channels + d)*out_width*out_height + oh*out_width + ow] = bdhwsum;
                }
            }
        }
    }
    
    }
    
}

template <class T>
void acc_relu(const Tensor4D<T> &input, Tensor4D<T> *output) {
    int size = input.size();
    
    
    const T *in_data = input.data();
    T *out_data = output->data();
    
    #pragma acc data present(in_data[:size]) present(out_data[:size])
    {
    #pragma acc parallel loop
    for(int j = 0; j < size; j++) {
        T val = in_data[j];

        if(val > 0) {
            out_data[j] = val;
        }
        else {
            out_data[j] = (T)0.0f;
        }
    }
    }
}

template<class T>
void acc_relu_backprop(const Tensor4D<T> &drv_error_output, const Tensor4D<T> &output, Tensor4D<T> *drv_error_output_preact) {
    Shape4D output_preact_shape = drv_error_output_preact->shape();
    
    assert(output_preact_shape == drv_error_output.shape());
    assert(output_preact_shape == output.shape());
    
    Shape4D flatshape = output_preact_shape.flat(1);
    int B = flatshape[0], M = flatshape[1];
    
    const T *drv_error_output_data = drv_error_output.data(), *output_data = output.data();
    T *drv_error_output_preact_data = drv_error_output_preact->data();
    
    #pragma acc parallel loop collapse(2) present(drv_error_output_data[:B*M]) present(output_data[:B*M]) present(drv_error_output_preact_data[:B*M])
    for(int i = 0; i < B; i++) {
        for(int j = 0; j < M; j++) {
            double output_m = output_data[i*M + j];
            if (output_m>0) {
                drv_error_output_preact_data[i*M + j] = drv_error_output_data[i*M + j];
            }
            else {
                drv_error_output_preact_data[i*M + j] = 0.0f;
            }
        }
    }
}

template <class T>
void acc_sigmoid(const Tensor4D<T> &input, Tensor4D<T> *output) {
    int size = input.size();
    
    const T *in_data = input.data();
    T *out_data = output->data();
    
    #pragma acc data present(in_data[:size]) present(out_data[:size])
    {
    #pragma acc parallel loop
    for(int j = 0; j < size; j++) {
        T val = 1/(1 + exp(-1 * in_data[j]));

        out_data[j] = val;
    }
    }
}

//TODO template with constexpr for any non-softmax activations

template<class T>
void acc_sigmoid_backprop(const Tensor4D<T> &drv_error_output, const Tensor4D<T> &output, Tensor4D<T> *drv_error_output_preact) {
    Shape4D output_preact_shape = drv_error_output_preact->shape();
    
    assert(output_preact_shape == drv_error_output.shape());
    assert(output_preact_shape == output.shape());
    
    Shape4D flatshape = output_preact_shape.flat(1);
    int B = flatshape[0], M = flatshape[1];
    
    const T *drv_error_output_data = drv_error_output.data(), *output_data = output.data();
    T *drv_error_output_preact_data = drv_error_output_preact->data();
    
    #pragma acc parallel loop collapse(2) present(drv_error_output_data[:B*M]) present(output_data[:B*M]) present(drv_error_output_preact_data[:B*M])
    for(int i = 0; i < B; i++) {
        for(int j = 0; j < M; j++) {
            double output_m = output_data[i*M + j];
            drv_error_output_preact_data[i*M + j] = drv_error_output_data[i*M + j] * (output_m) * (1-output_m);
        }
    }
}

template <class T>
void acc_softmax(const Tensor4D<T> &input, Tensor4D<T> *output) {
    Shape4D data_shape = input.shape().flat(1);
    int size = data_shape.size();
    int B = data_shape[0];
    int M = data_shape[1];
    
    const T *in_data = input.data();
    T *out_data = output->data();
        
    #pragma acc data present(in_data[:size]) present(out_data[:size])
    {
    
    #pragma acc parallel loop
    for(int i = 0; i < B; i++) {
        T outsumi = 0.0f;
        #pragma acc loop reduction(+:outsumi)
        for(int j = 0; j < M; j++) {
            outsumi += exp(in_data[i*M + j]);
            out_data[i*M + j] = exp(in_data[i*M + j]);
        }
        #pragma acc loop
        for(int j = 0; j < M; j++) {
            out_data[i*M + j] /= outsumi;
        }
    }
    
    }
}

template<class T>
void acc_softmax_backprop(const Tensor4D<T> &drv_error_output, const Tensor4D<T> &output, Tensor4D<T> *drv_error_output_preact) {
    Shape4D output_preact_shape = drv_error_output_preact->shape();
    
    assert(output_preact_shape == drv_error_output.shape());
    assert(output_preact_shape == output.shape());
    
    Shape4D flatshape = output_preact_shape.flat(1);
    int B = flatshape[0], M = flatshape[1];
    
    const T *drv_error_output_data = drv_error_output.data(), *output_data = output.data();
    T *drv_error_output_preact_data = drv_error_output_preact->data();
    
    #pragma acc parallel loop collapse(2) present(drv_error_output_data[:B*M]) present(output_data[:B*M]) present(drv_error_output_preact_data[:B*M])
    for(int i = 0; i < B; i++) {
        for(int j = 0; j < M; j++) {
            double drv_error_output_preact_j = 0.0f;
            double output_j = output_data[i*M + j];
            
            #pragma acc loop reduction(+:drv_error_output_preact_j)
            for(int k = 0; k < M; k++) {
                double drv_output_k_preact_j;
                double drv_error_output_k;
                
                #pragma acc atomic read
                drv_error_output_k = drv_error_output_data[i*M + k];
                
                if(k==j) {
                    drv_output_k_preact_j = (output_j)*(1 - output_j);
                }
                else {
                    double output_k;
                    #pragma acc atomic read
                    output_k = output_data[i*M + k];
                    drv_output_k_preact_j = -1.0f * output_j * output_k;
                }
                
                drv_error_output_preact_j += drv_error_output_k * drv_output_k_preact_j;
            }
            
            drv_error_output_preact_data[i*M + j] = drv_error_output_preact_j;
        }
    }
    
}


template <class T>
void acc_pad2D_inner(const Tensor4D<T> &pre_pad, Tensor4D<T> *post_pad, int padding_top, int padding_bottom, int padding_left, int padding_right, int padding_inner_rows, int padding_inner_columns) {
    LOG("acc_pad2D_inner");
    
    Shape4D pre_pad_shape = pre_pad.shape(), post_pad_shape = post_pad->shape();
    
    int B = pre_pad_shape[0], C = pre_pad_shape[1], N = pre_pad_shape[2], M = pre_pad_shape[3];
    int padded_N = post_pad_shape[2], padded_M = post_pad_shape[3];
    
    assert(padded_N==(N + padding_top + padding_bottom + (N-1)*padding_inner_rows));
    assert(padded_M==(M + padding_left + padding_right + (M-1)*padding_inner_columns));

    const T *pre_pad_data = pre_pad.data();
    T *post_pad_data = post_pad->data();
    
    LOG("Entering loop collapse(4)");
    #pragma acc data present(pre_pad_data[:(B*C*M*N)], post_pad_data[:(B*C*padded_N*padded_M)])
    {
    acc_zeros(post_pad);
    #pragma acc parallel loop collapse(4)
    for(int b = 0; b < B; b++) {
        for(int c = 0; c < C; c++) {
            for(int i = 0; i < N; i++) {
                for(int j = 0; j < M; j++) {
                    #ifdef SAFEDATA
                    printf("Post_pad[%d, %d, %d, %d] = Pre_pad[%d, %d, %d, %d](%+11.5f)", b, c, i+padding_top, j+padding_left, b, c, i, j, pre_pad_data[b*C*M*N + c*M*N + i*M + j]);
                    #endif
                    
                    post_pad_data[b*padded_N*padded_M * C + c*padded_N*padded_M + (i+padding_top + i*padding_inner_rows)*padded_M + (j + padding_left + j*padding_inner_columns)] = pre_pad_data[b*C*M*N + c*M*N + i*M + j];
                    
                    #ifdef SAFEDATA
                    printf(" = %+11.5f\n", post_pad_data[b*padded_N*padded_M * C + c*padded_N*padded_M + (i+padding_top + i*padding_inner_rows)*padded_M + (j + padding_left + j*padding_inner_columns)]);
                    #endif
                }
            }
        }
    }
    
    }
}

template <class T>
void acc_pad2D(const Tensor4D<T> &pre_pad, Tensor4D<T> *post_pad, int padding_top, int padding_bottom, int padding_left, int padding_right) {
    acc_pad2D_inner(pre_pad, post_pad, padding_top, padding_bottom, padding_left, padding_right, 0, 0);
}

template <class T>
Tensor4D<T>* acc_padded2D_inner(const Tensor4D<T> &pre_pad, int padding_top, int padding_bottom, int padding_left, int padding_right, int padding_inner_rows, int padding_inner_columns) {
    Shape4D pre_pad_shape = pre_pad.shape();
    int B = pre_pad_shape[0], C = pre_pad_shape[1], N = pre_pad_shape[2], M = pre_pad_shape[3];
    
    
    Tensor4D<T> *ret = new Tensor4D<T>(B, C, N + padding_top + padding_bottom + (N-1)*padding_inner_rows, M + padding_left + padding_right + (M-1)*padding_inner_columns);
    ret->create_acc();
    LOG("acc_pad2D_inner(pre_pad, ret, padding_top, padding_bottom, padding_left, padding_right, padding_inner_rows, padding_inner_columns)");
    
    acc_pad2D_inner(pre_pad, ret, padding_top, padding_bottom, padding_left, padding_right, padding_inner_rows, padding_inner_columns);
    
    return ret;
}

template <class T>
void acc_rev_pad2D(const Tensor4D<T> &post_pad, Tensor4D<T> *pre_pad, int padding_top, int padding_bottom, int padding_left, int padding_right) {
    // acc_pad2D_inner(
    Shape4D pre_pad_shape = pre_pad->shape(), post_pad_shape = post_pad.shape();
    
    int B = pre_pad_shape[0], C = pre_pad_shape[1], N = pre_pad_shape[2], M = pre_pad_shape[3];
    int padded_N = post_pad_shape[2], padded_M = post_pad_shape[3];
    
    assert(padded_N==(N + padding_top + padding_bottom));
    assert(padded_M==(M + padding_left + padding_right));

    const T *post_pad_data = post_pad.data();
    T *pre_pad_data = pre_pad->data();
    
    #pragma acc data present(pre_pad_data[:(B*C*M*N)], post_pad_data[:(B*C*padded_N*padded_M)])
    {
        
    #pragma acc parallel loop collapse(4)
    for(int b = 0; b < B; b++) {
        for(int c = 0; c < C; c++) {
            for(int i = 0; i < N; i++) {
                for(int j = 0; j < M; j++) {
                    pre_pad_data[b*C*M*N + c*M*N + i*M + j] = post_pad_data[b*padded_N*padded_M * C + c*padded_N*padded_M + (i+padding_top)*padded_M + j + padding_left];
                }
            }
        }
    }
    
    }
}

template<class T>
void acc_normalize_img(Tensor4D<T> *output) {
    
    Shape4D output_shape = output->shape();
    
    int B = output_shape[0], input_size = output_shape[1]*output_shape[2]*output_shape[3];

    T *out_data = output->data();
    
    #pragma acc data present(out_data[:B*input_size])
    {
    #pragma acc parallel loop collapse(2)
    for(int i = 0; i < B; i++) {
        for(int k = 0; k < input_size; k++) {
            //bring values to [-0.5, 0.5]
            out_data[i*input_size + k] = ( out_data[i*input_size + k] -255.0f/2)/255.0f;
        }
    }
        
    }
}

template void acc_copy(const Tensor4D<double> &A, Tensor4D<double> *B);
template void acc_add(Tensor4D<double> *a, const Tensor4D<double> &b);
template void acc_val(Tensor4D<double> *A, double val);
template void acc_zeros(Tensor4D<double> *A);
template void acc_mltp(Tensor4D<double> *A, double mltp) ;
template void acc_accumulate(const Tensor4D<double> &, Tensor4D<double> *);
template void acc_rng(Tensor4D<double> *output, double mtlp);
template void acc_flip_spatial(Tensor4D<double> *input);
template void acc_matrix_multiply(const Tensor4D<double> &A, const Tensor4D<double> &B, Tensor4D<double> *C);
template void acc_convolution2D(const Tensor4D<double> &input, const Tensor4D<double> &filters, Tensor4D<double> *output, const vector<int> &stride);

template void acc_relu(const Tensor4D<double> &input, Tensor4D<double> *output);
template void acc_relu_backprop(const Tensor4D<double> &drv_error_output, const Tensor4D<double> &output, Tensor4D<double> *drv_error_output_preact);
template void acc_sigmoid(const Tensor4D<double> &input, Tensor4D<double> *output);
template void acc_sigmoid_backprop(const Tensor4D<double> &drv_error_output, const Tensor4D<double> &output, Tensor4D<double> *drv_error_output_preact);
template void acc_softmax(const Tensor4D<double> &input, Tensor4D<double> *output);
template void acc_softmax_backprop(const Tensor4D<double> &drv_error_output, const Tensor4D<double> &output, Tensor4D<double> *drv_error_output_preact);

template void acc_pad2D_inner(const Tensor4D<double> &pre_pad, Tensor4D<double> *post_pad, int padding_top, int padding_bottom, int padding_left, int padding_right, int padding_inner_rows, int padding_inner_columns);
template void acc_pad2D(const Tensor4D<double> &pre_pad, Tensor4D<double> *post_pad, int padding_top, int padding_bottom, int padding_left, int padding_right);
template Tensor4D<double>* acc_padded2D_inner(const Tensor4D<double> &pre_pad, int padding_top, int padding_bottom, int padding_left, int padding_right, int padding_inner_rows, int padding_inner_columns);
template void acc_rev_pad2D(const Tensor4D<double> &post_pad, Tensor4D<double> *pre_pad, int padding_top, int padding_bottom, int padding_left, int padding_right);
template void acc_normalize_img(Tensor4D<double> *output);

/////
