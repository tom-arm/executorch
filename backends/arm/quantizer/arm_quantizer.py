# Copyright (c) Meta Platforms, Inc. and affiliates.
# Copyright 2024-2025 Arm Limited and/or its affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

#
# Quantizer for Arm backend
#

from __future__ import annotations

import functools
from typing import Any, Callable, Dict, List, Optional

import torch
from executorch.backends.arm._passes import ArmPassManager

from executorch.backends.arm.quantizer import QuantizationConfig
from executorch.backends.arm.tosa_specification import TosaSpecification

from .arm_quantizer_utils import is_annotated, mark_node_as_annotated
from .quantization_annotator import annotate_graph
from executorch.backends.arm.arm_backend import (
    get_tosa_spec,
    is_ethosu,
    is_vgf,
)  # usort: skip
from executorch.exir.backend.compile_spec_schema import CompileSpec

from torch.fx import GraphModule, Node
from torchao.quantization.pt2e import (
    FakeQuantize,
    FusedMovingAvgObsFakeQuantize,
    HistogramObserver,
    MinMaxObserver,
    MovingAverageMinMaxObserver,
    ObserverOrFakeQuantizeConstructor,
    PerChannelMinMaxObserver,
    PlaceholderObserver,
)

from torchao.quantization.pt2e.quantizer import (
    annotate_input_qspec_map,
    annotate_output_qspec,
    QuantizationSpec,
    Quantizer,
)

__all__ = [
    "TOSAQuantizer",
    "EthosUQuantizer",
    "VgfQuantizer",
    "get_symmetric_quantization_config",
]


@functools.lru_cache
def get_symmetric_quantization_config(
    is_per_channel: bool = True,
    is_qat: bool = False,
    is_dynamic: bool = False,
    act_qmin: int = -128,
    act_qmax: int = 127,
    weight_qmin: int = -127,
    weight_qmax: int = 127,
):
    extra_args: Dict[str, Any] = {"eps": 2**-12}
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

    # Setup quantization config for weights
    weight_qscheme = (
        torch.per_channel_symmetric if is_per_channel else torch.per_tensor_symmetric
    )
    weight_observer_or_fake_quant_ctr: ObserverOrFakeQuantizeConstructor = (
        MinMaxObserver
    )
    # Determine the right observer/fake-quant constructor
    if is_qat:
        # Set plain fake-quant with true min/max
        weight_observer_or_fake_quant_ctr = FakeQuantize
    else:
        # PTQ: set min/max observer
        weight_observer_or_fake_quant_ctr = (
            PerChannelMinMaxObserver if is_per_channel else MinMaxObserver
        )

    extra_args = {"eps": 2**-12}

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
        )
    else:
        quantization_config = QuantizationConfig(
            act_quantization_spec,
            act_quantization_spec,
            weight_quantization_spec,
            bias_quantization_spec,
        )
    return quantization_config


NodeFilterType = Callable[[Node], bool]
"""Type for a Node Filter used by annotators. A Node filter is a function that takes
    a Node and returns whether the node should be annotated or not.
"""


def _get_module_name_filter(module_name: str) -> NodeFilterType:
    """Get the module_name_filter function for a given module name, the filter accepts
    a node and checks if the node comes from a module that has certain module name

    For example:
        node: linear_op = call_function[...](...)  # comes from a module with name blocks.sub.linear1

    >> module_name_filter = _get_module_name_filter("blocks.sub")
    >> print(module_name_filter(node))
    True  # the node is from "blocks.sub" based on the fully qualified name "blocks.sub.linear1"
    """

    name_start = len("L['self'].")

    def module_name_filter(n: Node) -> bool:
        # node_stack example: {
        #    'L__self___sub': ("L['self'].sub", <class '....Sub'>),
        #    'L__self___sub_linear': ("L['self'].sub.linear", <class 'torch.nn.modules.linear.Linear'>)
        # }
        # get_attr nodes doesn't have nn_module_stack?
        nn_module_stack = n.meta.get("nn_module_stack", {})
        names = [name[name_start:] for name, _ in nn_module_stack.values()]
        return module_name in names

    return module_name_filter


def _get_module_type_filter(tp: Callable) -> NodeFilterType:
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
        # node_stack example: {
        #     'L__self___sub': ("L['self'].sub", <class '....Sub'>),
        #     'L__self___sub_linear': ("L['self'].sub.linear", <class 'torch.nn.modules.linear.Linear'>)
        # }
        nn_module_stack = n.meta.get("nn_module_stack", {})
        types = [t for _, t in nn_module_stack.values()]
        return tp_str in types

    return module_type_filter


def _get_not_module_type_or_name_filter(
    tp_list: List[Callable], module_name_list: List[str]
) -> NodeFilterType:
    module_type_filters = [_get_module_type_filter(tp) for tp in tp_list]
    module_name_list_filters = [_get_module_name_filter(m) for m in module_name_list]

    def not_module_type_or_name_filter(n: Node) -> bool:
        return not any(f(n) for f in module_type_filters + module_name_list_filters)

    return not_module_type_or_name_filter


class TOSAQuantizer(Quantizer):

    def __init__(self, tosa_spec: TosaSpecification) -> None:
        super().__init__()
        self.tosa_spec = tosa_spec
        self.global_config: Optional[QuantizationConfig] = None
        self.io_config: Optional[QuantizationConfig] = None
        self.module_type_config: Dict[Callable, Optional[QuantizationConfig]] = {}
        self.module_name_config: Dict[str, Optional[QuantizationConfig]] = {}

    def set_global(self, quantization_config: QuantizationConfig) -> TOSAQuantizer:
        """Set quantization_config for submodules that are not already annotated by name or type filters."""
        self.global_config = quantization_config
        return self

    def set_module_type(
        self, module_type: Callable, quantization_config: QuantizationConfig
    ) -> TOSAQuantizer:
        """Set quantization_config for a submodule with type: `module_type`, for example:
        quantizer.set_module_name(Sub) or quantizer.set_module_name(nn.Linear), it will quantize all supported operator/operator
        patterns in the submodule with this module type with the given `quantization_config`
        """
        self.module_type_config[module_type] = quantization_config
        return self

    def set_module_name(
        self, module_name: str, quantization_config: Optional[QuantizationConfig]
    ) -> TOSAQuantizer:
        """Set quantization_config for a submodule with name: `module_name`, for example:
        quantizer.set_module_name("blocks.sub"), it will quantize all supported operator/operator
        patterns in the submodule with this module name with the given `quantization_config`
        """
        # Validate that quantization_config is provided
        if quantization_config is None:
            raise ValueError("quantization_config == None is not supported yet")
        self.module_name_config[module_name] = quantization_config
        return self

    def set_io(self, quantization_config):
        """Set quantization_config for input and output nodes."""
        self.io_config = quantization_config
        return self

    def transform_for_annotation(self, model: GraphModule) -> GraphModule:
        """An initial pass for transforming the graph to prepare it for annotation.
        Currently transforms scalar values to tensor attributes.
        """

        return ArmPassManager(self.tosa_spec).transform_for_annotation_pipeline(  # type: ignore[arg-type]
            graph_module=model
        )

    def annotate(self, model: GraphModule) -> GraphModule:
        """Performs the quantization annotation on the graph.
            Currently only does static quantization annotation.
        Args:
            model: The model to annotate statically.
        Returns:
            The annotated model.
        """
        model = self._annotate_for_static_quantization_config(model)
        return model

    def _annotate_all_static_patterns(
        self,
        model: GraphModule,
        quantization_config: Optional[QuantizationConfig],
        filter_fn: Optional[Callable[[Node], bool]] = None,
    ) -> GraphModule:
        """Loops over all STATIC_OPS and runs the corresponding registered annotator.
        Args:
            model: The model to annotate statically.
            quantization_config: Specifies the QuantizationSpecs for the model's
                input activations, output activations, weights and biases.
            filter_fn: An optional filter function that takes a node and returns whether the node should be annotated.
        Returns:
            The annotated model.
        """
        # TODO: implement the support for None to be canceling out previous annotations
        if quantization_config is None:
            return model

        annotate_graph(model, quantization_config, filter_fn)
        return model

    def _annotate_for_static_quantization_config(
        self, model: GraphModule
    ) -> GraphModule:
        """Matches the correct QuantizationConfig with the correct module using a filter
        when running _annotate_all_static_patterns.
        """
        if self.io_config:
            self._annotate_io(model, self.io_config)

        module_name_list = list(self.module_name_config.keys())
        for module_name, config in self.module_name_config.items():
            self._annotate_all_static_patterns(
                model, config, _get_module_name_filter(module_name)
            )

        tp_list = list(self.module_type_config.keys())
        for module_type, config in self.module_type_config.items():
            self._annotate_all_static_patterns(
                model, config, _get_module_type_filter(module_type)
            )

        self._annotate_all_static_patterns(
            model,
            self.global_config,
            _get_not_module_type_or_name_filter(tp_list, module_name_list),
        )

        return model

    def _annotate_io(
        self,
        model: GraphModule,
        quantization_config: QuantizationConfig,
    ):
        for node in model.graph.nodes:
            if is_annotated(node):
                continue
            if node.op == "placeholder" and len(node.users) > 0:
                annotate_output_qspec(
                    node,
                    quantization_config.get_output_act_qspec(),
                )
                mark_node_as_annotated(node)
            if node.op == "output":
                parent = node.all_input_nodes[0]
                annotate_input_qspec_map(
                    node, parent, quantization_config.get_input_act_qspec()
                )
                mark_node_as_annotated(node)

    def validate(self, model: GraphModule) -> None:
        pass


class EthosUQuantizer(TOSAQuantizer):
    def __init__(self, compile_spec: list[CompileSpec]) -> None:
        if not is_ethosu(compile_spec):
            raise RuntimeError("compile spec is not targeting Ethos-U")

        tosa_spec = get_tosa_spec(compile_spec)
        super().__init__(tosa_spec)


class VgfQuantizer(TOSAQuantizer):
    def __init__(self, compile_spec: list[CompileSpec]) -> None:
        if not is_vgf(compile_spec):
            raise RuntimeError("compile spec is not targeting VGF")

        tosa_spec = get_tosa_spec(compile_spec)
        super().__init__(tosa_spec)
