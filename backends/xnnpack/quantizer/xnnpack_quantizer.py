# mypy: allow-untyped-defs
from __future__ import annotations

import copy
import functools
from dataclasses import dataclass
from typing import Any, Callable, Optional, Set, TYPE_CHECKING

import torch
import torch._dynamo as torchdynamo
import torch.nn.functional as F
from executorch.backends.xnnpack.quantizer.xnnpack_quantizer_utils import (
    _convert_scalars_to_attrs,
    OP_TO_ANNOTATOR,
    propagate_annotation,
)
from torchao.quantization.pt2e import (
    FakeQuantize,
    FusedMovingAvgObsFakeQuantize,
    HistogramObserver,
    MinMaxObserver,
    MovingAverageMinMaxObserver,
    MovingAveragePerChannelMinMaxObserver,
    PerChannelMinMaxObserver,
    PlaceholderObserver,
)
from torchao.quantization.pt2e.quantizer import (
    get_module_name_filter,
    OperatorConfig,
    OperatorPatternType,
    QuantizationConfig,
    QuantizationSpec,
    Quantizer,
)


if TYPE_CHECKING:
    from torch.fx import Node
    from torchao.quantization.pt2e import ObserverOrFakeQuantizeConstructor


__all__ = [
    "XNNPACKQuantizer",
    "get_symmetric_quantization_config",
]


def _get_dynamo_graph(function: Callable, inputs) -> torch.fx.Graph:
    gm, _ = torchdynamo.export(function, aten_graph=True)(*inputs)
    gm.graph.eliminate_dead_code()
    return gm.graph


def _get_linear_patterns(input_size: list[int]):
    in_channels = input_size[-1]
    out_channels = 8  # hard coding but this should not matter
    weight = torch.ones((out_channels, in_channels))
    bias = torch.ones((out_channels,))
    act = torch.ones(input_size)

    def linear_op(act, weight, bias=None):
        return F.linear(act, weight, bias)

    pattern_w_bias = _get_dynamo_graph(linear_op, (act, weight, bias))
    pattern_wo_bias = _get_dynamo_graph(linear_op, (act, weight))
    return [pattern_w_bias, pattern_wo_bias]


def _supported_symmetric_quantized_operators() -> dict[str, list[OperatorPatternType]]:
    supported_operators: dict[str, list[OperatorPatternType]] = {
        # Both conv and linear should be able to handle relu + hardtanh fusion since
        # those are clamp ops
        "conv2d": [
            [torch.nn.Conv2d, torch.nn.ReLU],
            [torch.nn.Conv2d, F.relu],
            [F.conv2d, torch.nn.ReLU],
            [F.conv2d, F.relu],
        ],
        "linear": [[torch.nn.Linear], [F.linear]],
        "add": [[torch.add]],
        "adaptive_avg_pool2d": [
            [torch.nn.AdaptiveAvgPool2d],
            [F.adaptive_avg_pool2d],
        ],
    }
    return copy.deepcopy(supported_operators)


def _get_supported_symmetric_config_and_operators() -> list[OperatorConfig]:
    supported_config_and_operators: list[OperatorConfig] = []
    for quantization_config in [
        get_symmetric_quantization_config(),
        get_symmetric_quantization_config(is_qat=True),
        get_symmetric_quantization_config(is_per_channel=True),
        get_symmetric_quantization_config(is_per_channel=True, is_qat=True),
    ]:
        ops = _supported_symmetric_quantized_operators()
        supported_config_and_operators.extend(
            OperatorConfig(quantization_config, pattern_list)
            for pattern_list in ops.values()
        )
    return copy.deepcopy(supported_config_and_operators)


@functools.lru_cache
def get_symmetric_quantization_config(
    is_per_channel: bool = False,
    is_qat: bool = False,
    is_dynamic: bool = False,
    act_qmin: int = -128,
    act_qmax: int = 127,
    weight_qmin: int = -127,
    weight_qmax: int = 127,
):
    extra_args: dict[str, Any] = {"eps": 2**-12}
    if is_qat:
        if is_dynamic:
            act_observer_or_fake_quant_ctr = FakeQuantize
            dynamic_quant_observer = MovingAverageMinMaxObserver.with_args(
                averaging_constant=1
            )
            extra_args["observer"] = dynamic_quant_observer
        else:
            act_observer_or_fake_quant_ctr = FusedMovingAvgObsFakeQuantize  # type: ignore[assignment]
    else:
        if is_dynamic:
            act_observer_or_fake_quant_ctr = PlaceholderObserver  # type: ignore[assignment]
        else:
            act_observer_or_fake_quant_ctr = HistogramObserver  # type: ignore[assignment]

    act_quantization_spec = QuantizationSpec(
        dtype=torch.int8,
        quant_min=act_qmin,
        quant_max=act_qmax,
        qscheme=torch.per_tensor_affine,
        is_dynamic=is_dynamic,
        observer_or_fake_quant_ctr=act_observer_or_fake_quant_ctr.with_args(
            **extra_args,
        ),
    )
    weight_qscheme = (
        torch.per_channel_symmetric if is_per_channel else torch.per_tensor_symmetric
    )
    weight_observer_or_fake_quant_ctr: ObserverOrFakeQuantizeConstructor = (
        MinMaxObserver
    )
    if is_qat:
        # TODO: qat + per channel?
        weight_observer_or_fake_quant_ctr = FusedMovingAvgObsFakeQuantize
    elif is_per_channel:
        weight_observer_or_fake_quant_ctr = PerChannelMinMaxObserver

    extra_args: dict[str, Any] = {"eps": 2**-12}
    if is_qat:
        if weight_qscheme == torch.per_tensor_symmetric:
            extra_args["observer"] = MovingAverageMinMaxObserver
        else:
            extra_args["observer"] = MovingAveragePerChannelMinMaxObserver  # type: ignore[dict-item]
    weight_quantization_spec = QuantizationSpec(
        dtype=torch.int8,
        quant_min=weight_qmin,
        quant_max=weight_qmax,
        qscheme=weight_qscheme,
        ch_axis=0,
        is_dynamic=False,
        observer_or_fake_quant_ctr=weight_observer_or_fake_quant_ctr.with_args(
            **extra_args
        ),
    )

    bias_quantization_spec = None
    if is_dynamic:
        quantization_config = QuantizationConfig(
            act_quantization_spec,
            None,
            weight_quantization_spec,
            bias_quantization_spec,
            is_qat,
        )
    else:
        quantization_config = QuantizationConfig(
            act_quantization_spec,
            act_quantization_spec,
            weight_quantization_spec,
            bias_quantization_spec,
            is_qat,
        )
    return quantization_config


def _get_supported_config_and_operators() -> list[OperatorConfig]:
    return _get_supported_symmetric_config_and_operators()


def _get_module_type_filter(tp: Callable):
    """Get the module_type_filter function for a given module type, the filter accepts
    a node and checks if the node comes from a module that has certain module type

    For example:
        node: linear_op = call_function[...](...)  # comes from a module with type Block -> Sub -> Linear


    >> module_type_filter = _get_module_type_filter(Sub)  # submodule with type `Sub`, under the `Block` submodule
    >> print(module_type_filter(node))
    True  # the node is from the submodule `Sub` (same for `Block` and `Linear` as well)
    """

    tp_str = tp.__module__ + "." + tp.__qualname__

    def module_type_filter(n: Node) -> bool:
        # example: {
        #     'L__self___sub': ("L['self'].sub", <class '....Sub'>),
        #     'L__self___sub_linear': ("L['self'].sub.linear", <class 'torch.nn.modules.linear.Linear'>)
        # }
        nn_module_stack = n.meta.get("nn_module_stack", {})
        types = []
        for _, t in nn_module_stack.values():
            # export() returns str, but older APIs (e.g. capture_pre_autograd_graph)
            # return type. Handle both cases.
            if isinstance(t, type):
                t = t.__module__ + "." + t.__qualname__
            types.append(t)
        return tp_str in types

    return module_type_filter


def _get_not_module_type_or_name_filter(
    tp_list: list[Callable], module_name_list: list[str]
) -> Callable[[Node], bool]:
    module_type_filters = [_get_module_type_filter(tp) for tp in tp_list]
    module_name_list_filters = [get_module_name_filter(m) for m in module_name_list]

    def not_module_type_or_name_filter(n: Node) -> bool:
        return not any(f(n) for f in module_type_filters + module_name_list_filters)

    return not_module_type_or_name_filter


@dataclass
class QuantPattern:
    name: str
    is_dynamic: bool
    is_qat: bool
    op_overloads: Set[torch._ops.OpOverloadPacket]


CONV_TARGETS = {
    torch.ops.aten.conv2d.default,
    torch.ops.aten.conv1d.default,
    torch.ops.aten.convolution.default,
}

CONV_TRANSPOSE_TARGETS = {
    torch.ops.aten.conv_transpose1d,
    torch.ops.aten.conv_transpose1d.default,
    torch.ops.aten.conv_transpose2d,
    torch.ops.aten.conv_transpose2d.input,
    torch.ops.aten.conv_transpose3d,
    torch.ops.aten.conv_transpose3d.input,
}

LINEAR_TARGETS = {
    torch.ops.aten.linear.default,
}

ADAPTIVE_AVG_POOL2D_TARGETS = {torch.ops.aten.adaptive_avg_pool2d.default}

ADD_TARGETS = {torch.ops.aten.add.Tensor}

MUL_TARGETS = {torch.ops.aten.mul.Tensor}

CAT_TARGETS = {torch.ops.aten.cat.default}


class XNNPACKQuantizer(Quantizer):
    supported_config_and_operators = _get_supported_config_and_operators()
    SUPPORTED_PATTERNS = [
        QuantPattern("conv_bn_relu", False, True, CONV_TARGETS),
        QuantPattern("conv_bn", False, True, CONV_TARGETS),
        QuantPattern("conv_transpose_bn_relu", False, True, CONV_TRANSPOSE_TARGETS),
        QuantPattern("conv_transpose_bn", False, True, CONV_TRANSPOSE_TARGETS),
        QuantPattern("linear_relu", False, False, LINEAR_TARGETS),
        QuantPattern("linear", True, False, LINEAR_TARGETS),
        QuantPattern("conv", True, False, CONV_TARGETS),
        QuantPattern("conv_transpose", True, False, CONV_TRANSPOSE_TARGETS),
        QuantPattern("conv_relu", False, False, CONV_TARGETS),
        QuantPattern("conv_transpose_relu", False, False, CONV_TRANSPOSE_TARGETS),
        QuantPattern("adaptive_avg_pool2d", False, False, ADAPTIVE_AVG_POOL2D_TARGETS),
        QuantPattern("add_relu", False, False, ADD_TARGETS),
        QuantPattern("add", False, False, ADD_TARGETS),
        QuantPattern("mul_relu", False, False, MUL_TARGETS),
        QuantPattern("mul", False, False, MUL_TARGETS),
        QuantPattern("cat", False, False, CAT_TARGETS),
    ]

    def __init__(self) -> None:
        super().__init__()
        self.global_config: Optional[QuantizationConfig] = None
        self.operator_type_config: dict[
            torch._ops.OpOverloadPacket, Optional[QuantizationConfig]
        ] = {}
        self.module_type_config: dict[Callable, Optional[QuantizationConfig]] = {}
        self.module_name_config: dict[str, Optional[QuantizationConfig]] = {}
        # If specified, only quantize nodes that return true for the filter
        # function.
        self.filter_fn: Optional[Callable[[Node], bool]] = None

    @classmethod
    def get_supported_quantization_configs(cls) -> list[QuantizationConfig]:
        op_configs: set[QuantizationConfig] = {
            spec for spec, _ in cls.supported_config_and_operators
        }
        return list(op_configs)

    @classmethod
    def get_supported_operator_for_quantization_config(
        cls, quantization_config: Optional[QuantizationConfig]
    ) -> list[OperatorPatternType]:
        if quantization_config is None:
            all_ops = []
            for _, ops in cls.supported_config_and_operators:
                all_ops.extend(ops)
            return all_ops

        for config, ops in cls.supported_config_and_operators:
            # note: this assumes each entry in cls.supported_spec_and_operators
            # corresponds to one spec, e.g. we don't have
            # [(spec1, op_list1), (spec1, op_list2), (spec2, op_list3)]
            # where the first and second entry have the same spec but did not
            # merge the op list
            if config == quantization_config:
                return ops
        return []

    def set_global(self, quantization_config: QuantizationConfig) -> XNNPACKQuantizer:
        self.global_config = quantization_config
        return self

    def set_operator_type(
        self,
        operator_type: torch._ops.OpOverloadPacket,
        quantization_config: QuantizationConfig,
    ) -> XNNPACKQuantizer:
        self.operator_type_config[operator_type] = quantization_config
        return self

    def set_module_type(
        self, module_type: Callable, quantization_config: QuantizationConfig
    ):
        """Set quantization_config for a submodule with type: `module_type`, for example:
        quantizer.set_module_name(Sub) or quantizer.set_module_name(nn.Linear), it will quantize all supported operator/operator
        patterns in the submodule with this module type with the given `quantization_config`
        """
        self.module_type_config[module_type] = quantization_config
        return self

    def set_module_name(
        self, module_name: str, quantization_config: Optional[QuantizationConfig]
    ):
        """Set quantization_config for a submodule with name: `module_name`, for example:
        quantizer.set_module_name("blocks.sub"), it will quantize all supported operator/operator
        patterns in the submodule with this module name with the given `quantization_config`
        """
        assert (
            quantization_config is not None
        ), " quantization_config == None is not supported yet"
        self.module_name_config[module_name] = quantization_config
        return self

    def set_filter_function(self, filter_fn: Callable[[Node], bool]):
        """
        Set the filter function. We only quantize nodes that return True for
        the filter function.
        """
        self.filter_fn = filter_fn
        return self

    def transform_for_annotation(
        self, model: torch.fx.GraphModule
    ) -> torch.fx.GraphModule:
        """Transforms scalar values to tensor attributes"""
        return _convert_scalars_to_attrs(model)

    def annotate(self, model: torch.fx.GraphModule) -> torch.fx.GraphModule:
        """just handling global spec for now"""
        model = self._annotate_for_quantization_config(model)
        propagate_annotation(model)
        return model

    def _annotate_all_patterns(
        self,
        model: torch.fx.GraphModule,
        quantization_config: Optional[QuantizationConfig],
        filter_fn: Optional[Callable[[Node], bool]] = None,
        operator_target: Optional[torch._ops.OpOverloadPacket] = None,
    ):
        # TODO: implement the support for None to be canceling out previous annotations
        if quantization_config is None:
            return model

        # Create a combined filter function, which returns True only when
        # both filter_fn and self.filter_fn return True.
        def combined_filter_fn(n: Node) -> bool:
            combined_filter = [self.filter_fn, filter_fn]
            return all(f(n) for f in combined_filter if f is not None)

        for pattern in self.SUPPORTED_PATTERNS:
            if operator_target and operator_target not in pattern.op_overloads:
                # if operator_target is specified, skip patterns that aren't
                # associated with that target
                continue
            if quantization_config.input_activation.is_dynamic and pattern.is_dynamic:
                OP_TO_ANNOTATOR[pattern.name](
                    model, quantization_config, combined_filter_fn
                )
            elif quantization_config.is_qat and pattern.is_qat:
                OP_TO_ANNOTATOR[pattern.name](
                    model, quantization_config, combined_filter_fn
                )
            elif not quantization_config.input_activation.is_dynamic:
                OP_TO_ANNOTATOR[pattern.name](
                    model, quantization_config, combined_filter_fn
                )

        return model

    def _annotate_for_quantization_config(
        self, model: torch.fx.GraphModule
    ) -> torch.fx.GraphModule:
        module_name_list = list(self.module_name_config.keys())
        for module_name, config in self.module_name_config.items():
            self._annotate_all_patterns(
                model, config, get_module_name_filter(module_name)
            )

        tp_list = list(self.module_type_config.keys())
        for module_type, config in self.module_type_config.items():
            self._annotate_all_patterns(
                model, config, _get_module_type_filter(module_type)
            )

        for op, config in self.operator_type_config.items():
            self._annotate_all_patterns(
                model,
                config,
                _get_not_module_type_or_name_filter(tp_list, module_name_list),
                op,
            )
        self._annotate_all_patterns(
            model,
            self.global_config,
            _get_not_module_type_or_name_filter(tp_list, module_name_list),
        )
        return model

    def validate(self, model: torch.fx.GraphModule) -> None:
        pass

    @classmethod
    def get_supported_operators(cls) -> list[OperatorConfig]:
        return cls.supported_config_and_operators
