#pragma once
#include "render/gpu/fl_gpu_context.h"
#include <causality.h>

/* Bind the editor's GPU driver to Flow's renderer contract; instance must outlive renderer resources. */
Fl_GpuContext fl_render_host_context(Ca_Instance *instance);
