# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.
# pylint: disable=invalid-name, unused-argument
"""Utility functions for relax training."""

from typing import Optional, Callable

import tvm
from tvm import relax
from tvm._ffi.registry import register_func
from tvm.relax.block_builder import BlockBuilder

from ..expr import Function, Var, Call
from . import _ffi_api


def AppendLoss(
    func_name: str,
    loss_function: Function,
    num_backbone_outputs: int = 1,
    new_func_name: Optional[str] = None,
) -> tvm.ir.transform.Pass:
    """Append the loss function to the backbone function specified by `func_name`. Generally, the
    loss function is generated by instances of `relax.training.Loss`.

    The backbone function and the loss function should satisfy a few restrictions:
    - Both backbone and loss should contain exactly one DataflowBlock.
    - Backbone should return either one Var, or a tuple of Vars
    - Loss should return a scalar(0-dim Tensor) Var

    They should be like:

    .. code-block:: python
        @R.function
        def backbone(input_instances, parameters, states):
            with R.dataflow():
                # Predicts the result
                ...
            return backbone_result, updated_states

        @R.function
        def loss(backbone_result, targets):
            with R.dataflow():
                # calculate the loss between backbone_result and targets
                ...
            # loss should be a scalar Var
            return loss

    Here each of input_instances, parameters, states, backbone_result and updated_states can
    denote a number of parameters.

    `states` denote the states that we need to maintain as the training process proceeds, such as
    the running mean and the running var of the batch norm operator. The updated states is returned
    in `updated_states`. States can be empty if there is no state that needs to be updated.

    The appended result contains only one DataflowBlock containing all bindings in backbone and
    loss. It will be like:

    .. code-block:: python
        @R.function
        def backbone_loss(input_instances, parameters, states, targets):
            with R.dataflow():
                # all bindings in backbone and loss
                ...
            return loss, updated_states

    Parameters
    ----------
    func_name : str
        The name of the backbone function in the IRModule.

    loss_func : Function
        The loss function.

    num_backbone_outputs : int
        Specify the number of `prediction_outputs` of the backbone function. Default: 1.

    new_func_name : Optional[str]
        Specify the name of the appended result. If is is not specified, the name will be
        `func_name + "_loss"`.

    Returns
    -------
    ret : Function
        The result function.

    Examples
    --------
    .. code-block:: python
        @I.ir_module
        class Module
            @R.function
            def predict(x: R.Tensor((2, 4), "float32"), y: R.Tensor((2, 4), "float32")):
                with R.dataflow():
                    out = R.add(x, y)
                    R.output(out)
                return out

        @R.function
        def loss(predictions: R.Tensor((2, 4), "float32"), labels: R.Tensor((2, 4), "float32")):
            with R.dataflow():
                lv = R.subtract(predictions, labels)
                lv1 = R.multiply(lv, lv)
                gv = R.sum(lv1)
                R.output(gv)
            return gv

        expected = AppendLoss("predict", loss)(Module)
        expected.show()

    Will get

    .. code-block:: python
        @I.ir_module
        class Module
            @R.function
            def predict(x: R.Tensor((2, 4), "float32"), y: R.Tensor((2, 4), "float32")):
                with R.dataflow():
                    out = R.add(x, y)
                    R.output(out)
                return out

            @R.function
            def predict_loss(x: R.Tensor((2, 4), "float32"), y: R.Tensor((2, 4), "float32"),
                             labels: R.Tensor((2, 4), "float32")) -> R.Tensor((), "float32"):
                with R.dataflow():
                    out: R.Tensor((2, 4), "float32") = R.add(x, y)
                    lv: R.Tensor((2, 4), "float32") = R.subtract(out, labels)
                    lv1: R.Tensor((2, 4), "float32") = R.multiply(lv, lv)
                    gv: R.Tensor((), "float32") = R.sum(lv1)
                    R.output(gv)
                return gv

    Notes
    -----
    This util can be replaced if we have inline pass. It is equivalent to inline a tail call in
    some sense.
    """

    return _ffi_api.AppendLoss(  # type: ignore
        func_name,
        loss_function,
        num_backbone_outputs,
        new_func_name,
    )


def register_te_gradient(te_grad_name: str, te_grad_func: Callable = None):
    """Register a te gradient function bind with name te_grad_name. te_grad_name can be referenced
    later in call_tir_with_grad nodes.

    Parameters
    ----------
    te_grad_name : str
        The registered name of the te gradient function. Should be align with the te_grad_name in
        call_tir_with_grad nodes.

    grad_func : Callable
        The te grad function.
        It must be a function taking (output_grad: Tensor, arg1: Tensor, arg2: Tensor, ...)
        as inputs and returning a list of Tensor created by te.compute.

    Returns
    -------
    mod : IRModule
        The mod with corresponding attributes attached.
    """

    def register(func: Callable):
        func_prefix = "tvm.relax.te_grad._register."

        # The handler function is used to let the backend (cpp side) to emit_te.
        # It's a wrapper of the te_grad_func.
        # It takes the blockbuilder, the gradient var of the output and the forward call expr.
        # It will return the emitted var.

        def handler(
            orig_var: Var, call_tir_with_grad: Call, output_grad: Var, ctx: BlockBuilder
        ) -> relax.Expr:
            return ctx.emit_te(
                func,
                output_grad,
                *call_tir_with_grad.args[1],
                **call_tir_with_grad.attrs.te_grad_kwargs,
                primfunc_name_hint=te_grad_name,
            )

        register_func(func_prefix + te_grad_name, handler)
        return func

    return register(te_grad_func) if te_grad_func else register
