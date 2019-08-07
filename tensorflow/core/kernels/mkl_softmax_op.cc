/* Copyright 2015 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

// See docs in ../ops/nn_ops.cc.
#ifdef INTEL_MKL

#include "mkldnn.hpp"
#include "tensorflow/core/framework/numeric_op.h"
#include "tensorflow/core/framework/op_kernel.h"
#include "tensorflow/core/framework/register_types.h"
#include "tensorflow/core/framework/tensor.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/util/mkl_util.h"
#include "tensorflow/core/util/tensor_format.h"
#include "third_party/eigen3/unsupported/Eigen/CXX11/Tensor"

using mkldnn::prop_kind;
using mkldnn::softmax_forward;
using mkldnn::stream;

namespace tensorflow {

class MklSoftmaxParams {
 public:
  memory::dims src_dims;
  memory::format src_fmt;
  int axis;

  MklSoftmaxParams(memory::dims src_dims, memory::format src_fmt, int axis)
      : src_dims(src_dims), src_fmt(src_fmt), axis(axis) {}
};

template <typename T>
class MklSoftmaxPrimitive : public MklPrimitive {
 public:
  explicit MklSoftmaxPrimitive(const MklSoftmaxParams& fwdParams)
      : cpu_engine_(engine::cpu, 0) {
    context_.fwd_stream.reset(new stream(stream::kind::eager));

    if (context_.softmax_fwd == nullptr) {
      Setup(fwdParams);
    }
  }

  ~MklSoftmaxPrimitive() {}

  // Softmax forward execute
  //   src_data:  input data buffer of src
  //   dst_data:  output data buffer of dst
  void Execute(const T* src_data, T* dst_data) {
    context_.src_mem->set_data_handle(
        static_cast<void*>(const_cast<T*>(src_data)));
    context_.dst_mem->set_data_handle(static_cast<void*>(dst_data));

    context_.fwd_stream->submit(context_.fwd_primitives);

    // after execution, set data handle back
    context_.src_mem->set_data_handle(DummyData);
    context_.dst_mem->set_data_handle(DummyData);
  }

  std::shared_ptr<mkldnn::softmax_forward::primitive_desc> GetSoftmaxFwdPd() {
    return context_.fwd_pd;
  }

 private:
  struct SoftmaxFwdContext {
    // MKLDNN memory
    std::shared_ptr<memory> src_mem;
    std::shared_ptr<memory> dst_mem;

    // desc & prmitive desc
    std::shared_ptr<mkldnn::softmax_forward::desc> fwd_desc;

    // memory desc
    std::shared_ptr<memory::desc> src_md;

    // Softmax primitive
    std::shared_ptr<mkldnn::softmax_forward::primitive_desc> fwd_pd;
    std::shared_ptr<mkldnn::primitive> softmax_fwd;

    std::shared_ptr<stream> fwd_stream;
    std::vector<mkldnn::primitive> fwd_primitives;

    SoftmaxFwdContext()
        : src_mem(nullptr),
          dst_mem(nullptr),
          fwd_desc(nullptr),
          src_md(nullptr),
          fwd_pd(nullptr),
          softmax_fwd(nullptr),
          fwd_stream(nullptr) {}
  };

  // Softmax forward primitive setup
  void Setup(const MklSoftmaxParams& fwdParams) {
    // create memory descriptors for softmax data with specified format
    context_.src_md.reset(new memory::desc({fwdParams.src_dims},
                                           MklDnnType<T>(), fwdParams.src_fmt));

    // create a softmax
    context_.fwd_desc.reset(new mkldnn::softmax_forward::desc(
        prop_kind::forward_scoring, *context_.src_md, fwdParams.axis));
    context_.fwd_pd.reset(new mkldnn::softmax_forward::primitive_desc(
        *context_.fwd_desc, cpu_engine_));

    // create memory primitive based on dummy data
    context_.src_mem.reset(
        new memory({*context_.src_md, cpu_engine_}, DummyData));
    context_.dst_mem.reset(
        new memory(context_.fwd_pd.get()->dst_primitive_desc(), DummyData));

    // create softmax primitive and add it to net
    context_.softmax_fwd.reset(new mkldnn::softmax_forward(
        *context_.fwd_pd, *context_.src_mem, *context_.dst_mem));

    context_.fwd_primitives.push_back(*context_.softmax_fwd);
  }

  struct SoftmaxFwdContext context_;
  engine cpu_engine_;
};

template <typename T>
class MklSoftmaxPrimitiveFactory : public MklPrimitiveFactory<T> {
 public:
  static MklSoftmaxPrimitive<T>* Get(const MklSoftmaxParams& fwdParams) {
    MklSoftmaxPrimitive<T>* softmax_forward = nullptr;

    // Get a softmax fwd primitive from the cached pool
    softmax_forward = static_cast<MklSoftmaxPrimitive<T>*>(
        MklSoftmaxPrimitiveFactory<T>::GetInstance().GetSoftmaxFwd(fwdParams));
    if (softmax_forward == nullptr) {
      softmax_forward = new MklSoftmaxPrimitive<T>(fwdParams);
      MklSoftmaxPrimitiveFactory<T>::GetInstance().SetSoftmaxFwd(
          fwdParams, softmax_forward);
    }
    return softmax_forward;
  }

  static MklSoftmaxPrimitiveFactory& GetInstance() {
    static MklSoftmaxPrimitiveFactory instance_;
    return instance_;
  }

 private:
  MklSoftmaxPrimitiveFactory() {}
  ~MklSoftmaxPrimitiveFactory() {}

  static string CreateKey(const MklSoftmaxParams& fwdParams) {
    string prefix = "softmax_fwd";
    FactoryKeyCreator key_creator;
    key_creator.AddAsKey(prefix);
    key_creator.AddAsKey(fwdParams.src_dims);
    key_creator.AddAsKey(fwdParams.axis);
    return key_creator.GetKey();
  }

  MklPrimitive* GetSoftmaxFwd(const MklSoftmaxParams& fwdParams) {
    string key = CreateKey(fwdParams);
    return this->GetOp(key);
  }

  void SetSoftmaxFwd(const MklSoftmaxParams& fwdParams, MklPrimitive* op) {
    string key = CreateKey(fwdParams);
    this->SetOp(key, op);
  }
};

typedef Eigen::ThreadPoolDevice CPUDevice;

template <typename Device, typename T>
class MklSoftmaxOp : public OpKernel {
 public:
  ~MklSoftmaxOp() {}

  explicit MklSoftmaxOp(OpKernelConstruction* context) : OpKernel(context) {}

  void Compute(OpKernelContext* context) override {
    try {
      // src_tensor now points to the 0-th input of global data struct "context"
      size_t src_idx = 0;
      const Tensor& src_tensor = MklGetInput(context, src_idx);
      // Add: get MklShape
      MklDnnShape src_mkl_shape;
      GetMklShape(context, src_idx, &src_mkl_shape);

      // src_dims is the dimension of src_tensor
      // dim of the dst will also be same as src_dims
      auto src_tf_shape = src_mkl_shape.IsMklTensor()
                              ? src_mkl_shape.GetTfShape()
                              : src_tensor.shape();
      const int input_dims = src_tf_shape.dims();
      memory::dims src_dims;
      int axis;
      if (src_mkl_shape.IsMklTensor()) {
        src_dims = src_mkl_shape.GetSizesAsMklDnnDims();
        axis = 1;
      } else {
        src_dims = TFShapeToMklDnnDims(src_tf_shape);
        axis = input_dims - 1;
      }
      memory::format layout_type;
      // In MKL, data format passed to mkl softmax op depends on dimension of
      // the input tensor. Here "x" data format in MKL is used for 1 dim tensor,
      // "nc" for 2 dim tensor, "tnc" for 3 dim tensor, "nchw" for 4 dim tensor,
      // and "ncdhw" for 5 dim tensor. Each of the symbols has the following
      // meaning: n = batch, c = channels, t = sequence length, h = height, w =
      // width, d = depth. When src tensor is MKL, layout_type here is only used
      // for setting TF layout type of output tensor. When input is TF Tensor,
      // layout here is no special sense. We use axis to define on which
      // dimension to do softmax.
      switch (input_dims) {
        case 1:
          layout_type = memory::format::x;
          break;
        case 2:
          layout_type = memory::format::nc;
          break;
        case 3:
          layout_type = memory::format::tnc;
          break;
        case 4:
          if (src_mkl_shape.IsMklTensor()) {
            layout_type = memory::format::nhwc;
          } else {
            layout_type = memory::format::nchw;
          }
          break;
        case 5:
          if (src_mkl_shape.IsMklTensor()) {
            layout_type = memory::format::ndhwc;
          } else {
            layout_type = memory::format::ncdhw;
          }
          break;
        default:
          OP_REQUIRES_OK(context,
                         errors::Aborted("Input dims must be <= 5 and >=1"));
          return;
      }

      // If input is in MKL layout, then simply grab input layout; otherwise,
      // construct input Tf layout. For TF layout, although input shape
      // (src_dims) required is in MKL-DNN order, the layout is Tensorflow's
      // layout
      auto src_fmt = src_mkl_shape.IsMklTensor()
                         ? static_cast<mkldnn::memory::format>(
                               src_mkl_shape.GetMklLayout().data.format)
                         : layout_type;

      // get a softmax fwd from primitive pool
      MklSoftmaxParams fwdParams(src_dims, src_fmt, axis);
      MklSoftmaxPrimitive<T>* softmax_fwd =
          MklSoftmaxPrimitiveFactory<T>::Get(fwdParams);

      // add: output
      Tensor* output_tensor = nullptr;
      MklDnnShape output_mkl_shape;
      TensorShape output_tf_shape;  // shape of output TF tensor.

      auto dst_pd = softmax_fwd->GetSoftmaxFwdPd()->dst_primitive_desc();

      // if input is MKL shape, output is also MKL shape.
      // if input is TF shape, output is also TF shape
      if (src_mkl_shape.IsMklTensor()) {
        output_mkl_shape.SetMklTensor(true);
        output_mkl_shape.SetMklLayout(&dst_pd);
        output_mkl_shape.SetElemType(MklDnnType<T>());
        output_mkl_shape.SetTfLayout(src_dims.size(), src_dims, layout_type);
        output_tf_shape.AddDim((dst_pd.get_size() / sizeof(T)));
      } else {  // then output is also TF shape
        output_mkl_shape.SetMklTensor(false);
        output_tf_shape = MklDnnDimsToTFShape(src_dims);
      }
      // Allocate output shape (MKL or TF based on the above)
      AllocateOutputSetMklShape(context, 0, &output_tensor, output_tf_shape,
                                output_mkl_shape);

      const T* src_data = src_tensor.flat<T>().data();
      T* dst_data = reinterpret_cast<T*>(output_tensor->flat<T>().data());

      // execute softmax
      softmax_fwd->Execute(src_data, dst_data);
    } catch (mkldnn::error& e) {
      string error_msg = "Status: " + std::to_string(e.status) + ", message: " +
                         string(e.message) + ", in file " + string(__FILE__) +
                         ":" + std::to_string(__LINE__);
      OP_REQUIRES_OK(
          context,
          errors::Aborted("Operation received an exception:", error_msg));
    }
  }
};

/* Register DNN kernels for supported operations and supported types - right now
 * it is only Softmax and f32 */
#define REGISTER_SOFTMAX_MKL_SUPPORTED_KERNELS_TYPES(type)     \
  REGISTER_KERNEL_BUILDER(                                     \
      Name("_MklSoftmax")                                      \
          .Device(DEVICE_CPU)                                  \
          .TypeConstraint<type>("T")                           \
          .Label(mkl_op_registry::kMklLayoutDependentOpLabel), \
      MklSoftmaxOp<CPUDevice, type>);
TF_CALL_float(REGISTER_SOFTMAX_MKL_SUPPORTED_KERNELS_TYPES);

}  // namespace tensorflow

#endif  // INTEL_MKL
