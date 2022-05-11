# Copyright (c) 2009-2022 The Regents of the University of Michigan.
# Part of HOOMD-blue, released under the BSD 3-Clause License.

"""Provide a tuner for `hoomd.md.nlist.NeighborList.buffer`."""

import copy

import numpy as np

import hoomd.custom
import hoomd.data
from hoomd.data.typeconverter import OnlyTypes, SetOnce
import hoomd.logging
import hoomd.tune
import hoomd.trigger
from hoomd.md.nlist import NeighborList


class _IntervalTPS:

    def __init__(self, simulation):
        self._simulation = simulation
        self._initial_timestep = simulation.initial_timestep
        self._last_timestep = simulation.timestep
        self._last_walltime = simulation.walltime
        self._last_tps = None

    def __call__(self):
        if self._simulation.timestep == self._last_timestep:
            return self._last_tps
        start = self._simulation.initial_timestep
        if start is not None and start > self._initial_timestep:
            self._initial_timestep = start
            # if condition is False then last call was the end of the last run
            # and we can tune.
            if self._last_timestep != start:
                return None
        walltime = self._simulation.walltime
        timestep = self._simulation.timestep
        if self._last_walltime is not None and self._last_timestep is not None:
            delta_w = walltime - self._last_walltime
            delta_t = timestep - self._last_timestep
            # We divide by 1_000 to reduce the gradient size for optimization.
            tps = delta_t / (1e3 * delta_w)
        else:
            tps = None
        self._last_tps = tps
        self._last_walltime = walltime
        self._last_timestep = timestep
        return tps


class _NeighborListBufferInternal(hoomd.custom._InternalAction):
    _skip_for_equality = {"_simulation", "_tunable"}

    def __init__(
        self,
        nlist: NeighborList,
        solver: hoomd.tune.solve.Optimizer,
        maximum_buffer: float,
    ):
        self._simulation = None
        self._tuned = False
        self._max_tps = 0
        self._best_buffer_size = None
        param_dict = hoomd.data.parameterdicts.ParameterDict(
            nlist=SetOnce(NeighborList),
            solver=SetOnce(hoomd.tune.solve.Optimizer),
            maximum_buffer=OnlyTypes(float,
                                     postprocess=self._maximum_buffer_post))
        param_dict.update({
            "nlist": nlist,
            "solver": solver,
            "maximum_buffer": maximum_buffer
        })
        self._param_dict.update(param_dict)

    def act(self, timestep: int):
        if not self.tuned:
            tps = self._tunable.y
            if tps is not None and tps > self._max_tps:
                self._best_buffer_size = self._tunable.x
                self._max_tps = tps
            self._tuned = self.solver.solve([self._tunable])

    def _maximum_buffer_post(self, value: float):
        if self._simulation is not None:
            self._tunable.domain = (1e-5, value)
        return value

    def attach(self, simulation):
        self._simulation = simulation
        self._tunable = self._make_tunable(self.nlist)

    def _make_tunable(self, nlist):
        return hoomd.tune.ManualTuneDefinition(
            get_y=_IntervalTPS(self._simulation),
            target=0.0,
            get_x=self._get_buffer,
            set_x=self._set_buffer,
            domain=(1e-5, self.maximum_buffer),
        )

    def _get_tps(self):
        if self._simulation.tps == 0:
            return None
        return self._simulation.tps

    def _get_buffer(self):
        return self.nlist.buffer

    def _set_buffer(self, new_buffer):
        self.nlist.buffer = new_buffer

    def detach(self):
        self._simulation = None
        self._tunable = None

    @property
    def tuned(self):
        return self._tuned

    @hoomd.logging.log
    def max_tps(self):
        return self._max_tps

    @hoomd.logging.log
    def best_buffer_size(self):
        return self._best_buffer_size

    def __getstate__(self):
        state = copy.copy(self.__dict__)
        for attr in self._skip_for_equality:
            state.pop(attr, None)
        return state


class NeighborListBuffer(hoomd.tune.custom_tuner._InternalCustomTuner):
    """Optimize neighbor list buffer size for maximum TPS.

    Direct instantiation of this class requires a `hoomd.tune.solve.RootStep`
    that determines how move sizes are updated. This class also provides class
    methods to create a `NeighborListBuffer` tuner with built-in solvers; see
    `NeighborListBuffer.with_grid` and
    `NeighborListBuffer.with_gradient_descent`.

    Args:
        trigger (hoomd.trigger.Trigger): ``Trigger`` to determine when to run
            the tuner.
        nlist (hoomd.md.nlist.NeighborLis): Neighbor list instance to tune.
        solver (`hoomd.tune.solve.Optimizer`): A solver that tunes the
        neighbor list buffer to maximize TPS.
        maximum_buffer (float): The largest buffer value to allow.

    Attributes:
        trigger (hoomd.trigger.Trigger): ``Trigger`` to determine when to run
            the tuner.
        solver (`hoomd.tune.solve.Optimizer`): A solver that tunes the
        neighbor list buffer to maximize TPS.
        maximum_buffer (float): The largest buffer value to allow.

    Warning:
        When using with a `hoomd.device.GPU` device, autotuning can mess with
        convergence. Running the simulation before adding the tuner to allow the
        autotuning to converge first results in better TPS optimization.
    """

    _internal_class = _NeighborListBufferInternal

    @classmethod
    def with_gradient_descent(
        cls,
        trigger: hoomd.trigger.Trigger,
        nlist: NeighborList,
        maximum_buffer: float,
        alpha: "hoomd.variant.Variant | float" = 0.01,
        kappa: "np.ndarray | None" = (0.33, 0.165),
        tol: float = 1e-5,
        max_delta: float = 0.05,
    ):
        """Create a `NeighborListBuffer` with a gradient descent solver.

        See `hoomd.tune.solve.GradientDescent` for more information on the
        solver.

        Args:
            trigger (hoomd.trigger.Trigger): ``Trigger`` to determine when to
                run the tuner.
            nlist (hoomd.md.nlist.NeighborList): Neighbor list buffer to
                maximize TPS.
            maximum_buffer (float): The largest buffer value to allow.
            alpha (`float`, optional): Real number between 0 and 1 used to
                dampen the rate of change in x (defaults to 0.1). ``alpha``
                scales the corrections to x each iteration.  Larger values of
                ``alpha`` lead to larger changes while a ``alpha`` of 0 leads to
                no change in x at all.
            kappa (`numpy.ndarray` of float, optional): A NumPy array of floats
                which are weight applied to the last :math:`N` of the gradients
                to add to the current gradient as well, where :math:`N` is the
                size of the array (defaults to `(0.33, 0.165)`).
            tol (`float`, optional): The absolute tolerance for convergence of
                y (defaults to 1e-5).
            max_delta (`float`, optional): The maximum iteration step allowed
                (defaults to 0.05).

        Note:
            Given the stocasticity of TPS, a non none ``kappa`` is recommended.

        Tip:
            When using the `hoomd.tune.solve.GradientDescent`, optimization is
            improved by starting at lower buffer values, as this has the
            steepest gradient.
        """
        if kappa is not None:
            kappa = np.array(kappa)
        return cls(
            trigger,
            nlist,
            hoomd.tune.solve.GradientDescent(alpha, kappa, tol, True,
                                             max_delta),
            maximum_buffer=maximum_buffer,
        )

    @classmethod
    def with_grid(
        cls,
        trigger: hoomd.trigger.Trigger,
        nlist: NeighborList,
        maximum_buffer: float,
        n_bins: int = 5,
        n_rounds: int = 1,
    ):
        """Create a `NeighborListBuffer` with a `hoomd.tune.GridOptimizer`.

        Args:
            trigger (hoomd.trigger.Trigger): ``Trigger`` to determine when to
                run the tuner.
            nlist (hoomd.md.nlist.NeighborList): Neighbor list buffer to
                maximize TPS.
            maximum_buffer (float): The largest buffer value to allow.
            n_bins (`int`, optional): The number of bins in the range to test
                (defaults to 2).
            n_rounds (`int`, optional): The number of rounds to perform the
                optimization over (defaults to 1).
        """
        return cls(
            trigger,
            nlist,
            hoomd.tune.GridOptimizer(n_bins, n_rounds, True),
            maximum_buffer=maximum_buffer,
        )
