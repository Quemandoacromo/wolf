#include "gst-video-context.hpp"
#include <gst/cuda/gstcudacontext.h>
#include <gst/cuda/gstcudautils.h>
#include <helpers/logger.hpp>

namespace gst_video_context {

using cuda_context_ptr = std::shared_ptr<GstCudaContext>;

struct GstVideoContext {
  cuda_context_ptr cuda_context;
  GstContext *context;
};

bool set_context(gst_context_ptr context, GstMessage *msg) {
  if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_NEED_CONTEXT) {
    const gchar *context_type;
    gst_message_parse_context_type(msg, &context_type);

    if (g_strcmp0(context_type, GST_CUDA_CONTEXT_TYPE) == 0) {
      gst_element_set_context(GST_ELEMENT(GST_MESSAGE_SRC(msg)), context->context);
      return true;
    }
    logs::log(logs::debug, "Received NEED_CONTEXT for type {}, but it is not supported", context_type);
  }
  return false;
}

cuda_context_ptr create_cuda_context(const std::string &device_path) {
  auto device_id = 0; // TODO: device_path to device_id
  auto cuda_ctx = gst_cuda_context_new(device_id);
  if (cuda_ctx) {
    return std::shared_ptr<GstCudaContext>(cuda_ctx, gst_object_unref);
  }
  logs::log(logs::warning, "Failed to create CUDA context for device: {}", device_path);
  return nullptr;
}

gst_context_ptr need_context_for_device(const std::string &device_path, GstMessage *msg) {
  if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_NEED_CONTEXT) {
    const gchar *context_type;
    gst_message_parse_context_type(msg, &context_type);

    logs::log(logs::debug, "Received NEED_CONTEXT for type {}", context_type);
    if (g_strcmp0(context_type, GST_CUDA_CONTEXT_TYPE) == 0) {
      if (auto cuda_context = create_cuda_context(device_path)) {
        auto context = gst_context_new_cuda_context(cuda_context.get());
        gst_element_set_context(GST_ELEMENT(GST_MESSAGE_SRC(msg)), context);
        logs::log(logs::debug, "Created CUDA context for device: {}", device_path);
        return std::make_shared<GstVideoContext>(GstVideoContext{
            .cuda_context = std::move(cuda_context),
            .context = context,
        });
      }
    }
  }

  return nullptr;
}

} // namespace gst_video_context