// Copyright (c) 2018 Google LLC.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "source/val/validate_scopes.h"

#include "source/diagnostic.h"
#include "source/spirv_target_env.h"
#include "source/val/instruction.h"
#include "source/val/validation_state.h"

namespace spvtools {
namespace val {

bool IsValidScope(uint32_t scope) {
  // Deliberately avoid a default case so we have to update the list when the
  // scopes list changes.
  switch (static_cast<SpvScope>(scope)) {
    case SpvScopeCrossDevice:
    case SpvScopeDevice:
    case SpvScopeWorkgroup:
    case SpvScopeSubgroup:
    case SpvScopeInvocation:
    case SpvScopeQueueFamilyKHR:
    case SpvScopeShaderCallKHR:
      return true;
    case SpvScopeMax:
      break;
  }
  return false;
}

spv_result_t ValidateScope(ValidationState_t& _, const Instruction* inst,
                           uint32_t scope) {
  SpvOp opcode = inst->opcode();
  bool is_int32 = false, is_const_int32 = false;
  uint32_t value = 0;
  std::tie(is_int32, is_const_int32, value) = _.EvalInt32IfConst(scope);

  if (!is_int32) {
    return _.diag(SPV_ERROR_INVALID_DATA, inst)
           << spvOpcodeString(opcode) << ": expected scope to be a 32-bit int";
  }

  if (!is_const_int32) {
    if (_.HasCapability(SpvCapabilityShader) &&
        !_.HasCapability(SpvCapabilityCooperativeMatrixNV)) {
      return _.diag(SPV_ERROR_INVALID_DATA, inst)
             << "Scope ids must be OpConstant when Shader capability is "
             << "present";
    }
    if (_.HasCapability(SpvCapabilityShader) &&
        _.HasCapability(SpvCapabilityCooperativeMatrixNV) &&
        !spvOpcodeIsConstant(_.GetIdOpcode(scope))) {
      return _.diag(SPV_ERROR_INVALID_DATA, inst)
             << "Scope ids must be constant or specialization constant when "
             << "CooperativeMatrixNV capability is present";
    }
  }

  if (is_const_int32 && !IsValidScope(value)) {
    return _.diag(SPV_ERROR_INVALID_DATA, inst)
           << "Invalid scope value:\n " << _.Disassemble(*_.FindDef(scope));
  }

  return SPV_SUCCESS;
}

spv_result_t ValidateExecutionScope(ValidationState_t& _,
                                    const Instruction* inst, uint32_t scope) {
  SpvOp opcode = inst->opcode();
  bool is_int32 = false, is_const_int32 = false;
  uint32_t value = 0;
  std::tie(is_int32, is_const_int32, value) = _.EvalInt32IfConst(scope);

  if (auto error = ValidateScope(_, inst, scope)) {
    return error;
  }

  if (!is_const_int32) {
    return SPV_SUCCESS;
  }

  // Vulkan specific rules
  if (spvIsVulkanEnv(_.context()->target_env)) {
    // Vulkan 1.1 specific rules
    if (_.context()->target_env != SPV_ENV_VULKAN_1_0) {
      // Scope for Non Uniform Group Operations must be limited to Subgroup
      if (spvOpcodeIsNonUniformGroupOperation(opcode) &&
          value != SpvScopeSubgroup) {
        return _.diag(SPV_ERROR_INVALID_DATA, inst)
               << _.VkErrorID(4642) << spvOpcodeString(opcode)
               << ": in Vulkan environment Execution scope is limited to "
               << "Subgroup";
      }
    }

    // OpControlBarrier must only use Subgroup execution scope for a subset of
    // execution models.
    if (opcode == SpvOpControlBarrier && value != SpvScopeSubgroup) {
      std::string errorVUID = _.VkErrorID(4682);
      _.function(inst->function()->id())
          ->RegisterExecutionModelLimitation([errorVUID](
                                                 SpvExecutionModel model,
                                                 std::string* message) {
            if (model == SpvExecutionModelFragment ||
                model == SpvExecutionModelVertex ||
                model == SpvExecutionModelGeometry ||
                model == SpvExecutionModelTessellationEvaluation ||
                model == SpvExecutionModelRayGenerationKHR ||
                model == SpvExecutionModelIntersectionKHR ||
                model == SpvExecutionModelAnyHitKHR ||
                model == SpvExecutionModelClosestHitKHR ||
                model == SpvExecutionModelMissKHR) {
              if (message) {
                *message =
                    errorVUID +
                    "in Vulkan environment, OpControlBarrier execution scope "
                    "must be Subgroup for Fragment, Vertex, Geometry, "
                    "TessellationEvaluation, RayGeneration, Intersection, "
                    "AnyHit, ClosestHit, and Miss execution models";
              }
              return false;
            }
            return true;
          });
    }

    // Only subset of execution models support Workgroup.
    if (value == SpvScopeWorkgroup) {
      std::string errorVUID = _.VkErrorID(4637);
      _.function(inst->function()->id())
          ->RegisterExecutionModelLimitation(
              [errorVUID](SpvExecutionModel model, std::string* message) {
                if (model != SpvExecutionModelTaskNV &&
                    model != SpvExecutionModelMeshNV &&
                    model != SpvExecutionModelTaskEXT &&
                    model != SpvExecutionModelMeshEXT &&
                    model != SpvExecutionModelTessellationControl &&
                    model != SpvExecutionModelGLCompute) {
                  if (message) {
                    *message =
                        errorVUID +
                        "in Vulkan environment, Workgroup execution scope is "
                        "only for TaskNV, MeshNV, TaskEXT, MeshEXT, "
                        "TessellationControl, and GLCompute execution models";
                  }
                  return false;
                }
                return true;
              });
    }

    // Vulkan generic rules
    // Scope for execution must be limited to Workgroup or Subgroup
    if (value != SpvScopeWorkgroup && value != SpvScopeSubgroup) {
      return _.diag(SPV_ERROR_INVALID_DATA, inst)
             << _.VkErrorID(4636) << spvOpcodeString(opcode)
             << ": in Vulkan environment Execution Scope is limited to "
             << "Workgroup and Subgroup";
    }
  }

  // TODO(atgoo@github.com) Add checks for OpenCL and OpenGL environments.

  // General SPIRV rules
  // Scope for execution must be limited to Workgroup or Subgroup for
  // non-uniform operations
  if (spvOpcodeIsNonUniformGroupOperation(opcode) &&
      value != SpvScopeSubgroup && value != SpvScopeWorkgroup) {
    return _.diag(SPV_ERROR_INVALID_DATA, inst)
           << spvOpcodeString(opcode)
           << ": Execution scope is limited to Subgroup or Workgroup";
  }

  return SPV_SUCCESS;
}

spv_result_t ValidateMemoryScope(ValidationState_t& _, const Instruction* inst,
                                 uint32_t scope) {
  const SpvOp opcode = inst->opcode();
  bool is_int32 = false, is_const_int32 = false;
  uint32_t value = 0;
  std::tie(is_int32, is_const_int32, value) = _.EvalInt32IfConst(scope);

  if (auto error = ValidateScope(_, inst, scope)) {
    return error;
  }

  if (!is_const_int32) {
    return SPV_SUCCESS;
  }

  if (value == SpvScopeQueueFamilyKHR) {
    if (_.HasCapability(SpvCapabilityVulkanMemoryModelKHR)) {
      return SPV_SUCCESS;
    } else {
      return _.diag(SPV_ERROR_INVALID_DATA, inst)
             << spvOpcodeString(opcode)
             << ": Memory Scope QueueFamilyKHR requires capability "
             << "VulkanMemoryModelKHR";
    }
  }

  if (value == SpvScopeDevice &&
      _.HasCapability(SpvCapabilityVulkanMemoryModelKHR) &&
      !_.HasCapability(SpvCapabilityVulkanMemoryModelDeviceScopeKHR)) {
    return _.diag(SPV_ERROR_INVALID_DATA, inst)
           << "Use of device scope with VulkanKHR memory model requires the "
           << "VulkanMemoryModelDeviceScopeKHR capability";
  }

  // Vulkan Specific rules
  if (spvIsVulkanEnv(_.context()->target_env)) {
    if (value != SpvScopeDevice && value != SpvScopeWorkgroup &&
        value != SpvScopeSubgroup && value != SpvScopeInvocation &&
        value != SpvScopeShaderCallKHR && value != SpvScopeQueueFamily) {
      return _.diag(SPV_ERROR_INVALID_DATA, inst)
             << _.VkErrorID(4638) << spvOpcodeString(opcode)
             << ": in Vulkan environment Memory Scope is limited to Device, "
                "QueueFamily, Workgroup, ShaderCallKHR, Subgroup, or "
                "Invocation";
    } else if (_.context()->target_env == SPV_ENV_VULKAN_1_0 &&
               value == SpvScopeSubgroup &&
               !_.HasCapability(SpvCapabilitySubgroupBallotKHR) &&
               !_.HasCapability(SpvCapabilitySubgroupVoteKHR)) {
      return _.diag(SPV_ERROR_INVALID_DATA, inst)
             << _.VkErrorID(6997) << spvOpcodeString(opcode)
             << ": in Vulkan 1.0 environment Memory Scope is can not be "
                "Subgroup without SubgroupBallotKHR or SubgroupVoteKHR "
                "declared";
    }

    if (value == SpvScopeShaderCallKHR) {
      std::string errorVUID = _.VkErrorID(4640);
      _.function(inst->function()->id())
          ->RegisterExecutionModelLimitation(
              [errorVUID](SpvExecutionModel model, std::string* message) {
                if (model != SpvExecutionModelRayGenerationKHR &&
                    model != SpvExecutionModelIntersectionKHR &&
                    model != SpvExecutionModelAnyHitKHR &&
                    model != SpvExecutionModelClosestHitKHR &&
                    model != SpvExecutionModelMissKHR &&
                    model != SpvExecutionModelCallableKHR) {
                  if (message) {
                    *message =
                        errorVUID +
                        "ShaderCallKHR Memory Scope requires a ray tracing "
                        "execution model";
                  }
                  return false;
                }
                return true;
              });
    }

    if (value == SpvScopeWorkgroup) {
      std::string errorVUID = _.VkErrorID(4639);
      _.function(inst->function()->id())
          ->RegisterExecutionModelLimitation(
              [errorVUID](SpvExecutionModel model, std::string* message) {
                if (model != SpvExecutionModelGLCompute &&
                    model != SpvExecutionModelTaskNV &&
                    model != SpvExecutionModelMeshNV &&
                    model != SpvExecutionModelTaskEXT &&
                    model != SpvExecutionModelMeshEXT) {
                  if (message) {
                    *message = errorVUID +
                               "Workgroup Memory Scope is limited to MeshNV, "
                               "TaskNV, MeshEXT, TaskEXT and GLCompute "
                               "execution model";
                  }
                  return false;
                }
                return true;
              });
    }
  }

  // TODO(atgoo@github.com) Add checks for OpenCL and OpenGL environments.

  return SPV_SUCCESS;
}

}  // namespace val
}  // namespace spvtools
