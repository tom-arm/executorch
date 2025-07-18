# Copyright 2024-2025 Arm Limited and/or its affiliates.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

from typing import Any, List

import numpy as np
import torch
from executorch.backends.arm.operators.node_visitor import (
    NodeVisitor,
    register_node_visitor,
)
from executorch.backends.arm.operators.operator_validation_utils import (
    validate_num_inputs,
    validate_valid_dtype,
)
from executorch.backends.arm.tosa_mapping import TosaArg

from executorch.backends.arm.tosa_specification import TosaSpecification


@register_node_visitor
class TableVisitor_0_80(NodeVisitor):
    target = "_table.default"

    tosa_specs = NodeVisitor.tosa_specs_0_80

    def define_node(
        self,
        node: torch.fx.Node,
        tosa_graph: Any,
        inputs: List[TosaArg],
        output: TosaArg,
    ) -> None:
        import tosa_tools.v0_80.serializer.tosa_serializer as ts  # type: ignore

        validate_num_inputs(self.target, inputs, 1)
        validate_valid_dtype(
            self.target, inputs, [ts.DType.INT8, ts.DType.INT16], output.tosa_spec
        )
        if inputs[0].dtype == ts.DType.INT8:
            validate_valid_dtype(self.target, output, ts.DType.INT8, output.tosa_spec)
        if inputs[0].dtype == ts.DType.INT16:
            validate_valid_dtype(self.target, output, ts.DType.INT32, output.tosa_spec)

        if node.name not in self._exported_program.state_dict.keys():  # type: ignore[union-attr]
            raise RuntimeError(
                f"Did not find key {node.name} in state_dict {self._exported_program.state_dict.keys()}."
            )

        table = self._exported_program.state_dict[node.name]  # type: ignore[union-attr]
        table_attr = ts.TosaSerializerAttribute()
        table_attr.TableAttribute(np.array(table))

        tosa_graph.addOperator(
            ts.TosaOp.Op().TABLE, [inputs[0].name], [output.name], table_attr
        )


@register_node_visitor
class TableVisitor(NodeVisitor):
    target = "_table.default"

    tosa_specs = [TosaSpecification.create_from_string("TOSA-1.0+INT")]

    def define_node(
        self,
        node: torch.fx.Node,
        tosa_graph: Any,
        inputs: List[TosaArg],
        output: TosaArg,
    ) -> None:
        import serializer.tosa_serializer as ts  # type: ignore

        validate_num_inputs(self.target, inputs, 1)
        validate_valid_dtype(
            self.target, inputs, [ts.DType.INT8, ts.DType.INT16], output.tosa_spec
        )
        if inputs[0].dtype == ts.DType.INT8:
            validate_valid_dtype(self.target, output, ts.DType.INT8, output.tosa_spec)
        if inputs[0].dtype == ts.DType.INT16:
            validate_valid_dtype(self.target, output, ts.DType.INT32, output.tosa_spec)

        if node.name not in self._exported_program.state_dict.keys():  # type: ignore[union-attr]
            raise RuntimeError(
                f"Did not find key {node.name} in state_dict {self._exported_program.state_dict.keys()}."
            )

        table = self._exported_program.state_dict[node.name]

        table_tensor_name = node.name + "_table"
        tosa_graph.addConst(
            table.shape,
            ts.DType.INT8 if inputs[0].dtype == ts.DType.INT8 else ts.DType.INT16,
            table.detach().numpy(),
            name=table_tensor_name,
        )

        tosa_graph.addOperator(
            ts.TosaOp.Op().TABLE,
            [inputs[0].name, table_tensor_name],
            [output.name],
            None,
        )
