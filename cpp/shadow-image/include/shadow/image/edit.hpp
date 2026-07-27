#pragma once

// Compatibility and navigation entry point for the edit kernel. Production code should include
// the narrow semantic owner directly; see cpp/shadow-image/README.md for the ownership map.
#include <shadow/image/adjustment_graph.hpp>
#include <shadow/image/adjustment_layers.hpp>
#include <shadow/image/adjustment_parameters.hpp>
#include <shadow/image/cpu_edit_reference.hpp>
#include <shadow/image/edit_error.hpp>
#include <shadow/image/edit_execution_plan.hpp>
#include <shadow/image/edited_proxy_rendering.hpp>
#include <shadow/image/full_edit_detail.hpp>
#include <shadow/image/photo_geometry.hpp>
#include <shadow/image/proxy_rendering.hpp>
#include <shadow/image/retouch.hpp>
#include <shadow/image/warm_edit_preview.hpp>
#include <shadow/image/working_rgb.hpp>
