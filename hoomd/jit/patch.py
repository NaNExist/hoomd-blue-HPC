# Copyright (c) 2009-2021 The Regents of the University of Michigan
# This file is part of the HOOMD-blue project, released under the BSD 3-Clause License.

import hoomd
from hoomd.hpmc import integrate
from hoomd import _hoomd
from hoomd.jit import _jit
from hoomd.operation import Compute
from hoomd.data.parameterdicts import TypeParameterDict, ParameterDict
from hoomd.data.typeparam import TypeParameter
from hoomd.logging import log

import subprocess
import os

import numpy as np

class PatchCompute(Compute):
    """Base class for HOOMD patch interaction computes. Provides helper
       methods to compile the user code in both CPU and GPU devices.

    Note:
        This class should not be instantiated by users. The class can be used
        for `isinstance` or `issubclass` checks.
    """

    def __init__(self, r_cut, array_size=1, log_only=False,
                 code=None, llvm_ir_file=None, clang_exec=None):
        param_dict = ParameterDict(r_cut = r_cut,
                                   log_only = log_only)
        self._param_dict.update(param_dict)
        # these only exist on python
        self._code = code
        self._array_size = array_size
        self._llvm_ir_file = llvm_ir_file
        self._clang_exec = clang_exec
        self.alpha_iso = np.zeros(array_size)

    def _attach(self):
        super()._attach()

    @property
    def array_size(self):
        return self._array_size

    @array_size.setter
    def array_size(self, size):
        if self._attached():
            raise AttributeError("This attribute can only be set when the patch is not attached.")
        else:
            self._array_size = size

    @log
    def energy(self):
        """float: Total interaction energy of the system in the current state.
                  Returns `None` when the patch object and integrator are not attached.
        """
        integrator = self._simulation.operations.integrator
        if self._attached and integrator._attached:
            timestep = self._simulation.timestep
            return integrator._cpp_obj.computePatchEnergy(timestep)
        else:
            return None

    @property
    def code(self):
        return self._code

    @code.setter
    def code(self, code):
        if self._attached():
            raise AttributeError("This attribute can only be set when the patch is not attached.")
        else:
            self._code = code

    @property
    def llvm_ir_file(self):
        return self._llvm_ir_file

    @llvm_ir_file.setter
    def llvm_ir_file(self, llvm_ir):
        if self._attached():
            raise AttributeError("This attribute can only be set when the patch is not attached.")
        else:
            self._llvm_ir_file = llvm_ir

    @property
    def clang_exec(self):
        return self._clang_exec

    @clang_exec.setter
    def clang_exec(self, clang):
        if self._attached():
            raise AttributeError("This attribute can only be set when the patch is not attached.")
        else:
            self._clang_exec = clang

    def _setup_gpu_code_path(self):
        include_path_hoomd = os.path.dirname(hoomd.__file__) + '/include';
        include_path_source = hoomd.version.source_dir
        include_path_cuda = _jit.__cuda_include_path__
        self._options = ["-I"+include_path_hoomd, "-I"+include_path_source, "-I"+include_path_cuda]
        self._cuda_devrt_library_path = _jit.__cuda_devrt_library_path__

        # select maximum supported compute capability out of those we compile for
        self._compute_archs = _jit.__cuda_compute_archs__;
        compute_archs_vec = _hoomd.std_vector_uint()
        compute_capability = cpp_exec_conf.getComputeCapability(0) # GPU 0
        compute_major, compute_minor = compute_capability.split('.')
        self._max_arch = 0
        for a in self._compute_archs.split('_'):
            if int(a) < int(compute_major)*10+int(compute_major):
                self._max_arch = int(a)

    def _compile_user(self, code, clang_exec, fn=None):
        R'''Helper function to compile the provided code into an executable

        Args:
            code (str): C++ code to compile
            clang_exec (str): The Clang executable to use
            fn (str): If provided, the code will be written to a file.

        .. versionadded:: 2.3
        '''
        cpp_function =  """
                        #include <stdio.h>
                        #include "hoomd/HOOMDMath.h"
                        #include "hoomd/VectorMath.h"

                        // these are allocated by the library
                        float *alpha_iso;
                        float *alpha_union;

                        extern "C"
                        {
                        float eval(const vec3<float>& r_ij,
                            unsigned int type_i,
                            const quat<float>& q_i,
                            float d_i,
                            float charge_i,
                            unsigned int type_j,
                            const quat<float>& q_j,
                            float d_j,
                            float charge_j)
                            {
                        """
        cpp_function += code
        cpp_function += """
                            }
                        }
                        """

        include_path = os.path.dirname(hoomd.__file__) + '/include';
        include_path_source = hoomd.version.source_dir

        if clang_exec is not None:
            clang = clang_exec;
        else:
            clang = 'clang';

        if fn is not None:
            cmd = [clang, '-O3', '--std=c++14', '-DHOOMD_LLVMJIT_BUILD', '-I', include_path, '-I', include_path_source, '-S', '-emit-llvm','-x','c++', '-o',fn,'-']
        else:
            cmd = [clang, '-O3', '--std=c++14', '-DHOOMD_LLVMJIT_BUILD', '-I', include_path, '-I', include_path_source, '-S', '-emit-llvm','-x','c++', '-o','-','-']
        p = subprocess.Popen(cmd,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE)

        # pass C++ function to stdin
        output = p.communicate(cpp_function.encode('utf-8'))
        llvm_ir = output[0].decode()

        if p.returncode != 0:
            self._simulation.device._cpp_msg.error("Error compiling provided code\n");
            self._simulation.device._cpp_msg.error("Command "+' '.join(cmd)+"\n");
            self._simulation.device._cpp_msg.error(output[1].decode()+"\n");
            raise RuntimeError("Error initializing patch energy");

        return llvm_ir

    def _wrap_gpu_code(self, code):
        R'''Helper function to compile the provided code into a device function

        Args:
            code (str): C++ code to compile

        .. versionadded:: 3.0
        '''
        cpp_function =  """
                        #include "hoomd/HOOMDMath.h"
                        #include "hoomd/VectorMath.h"
                        #include "hoomd/hpmc/IntegratorHPMCMonoGPUJIT.inc"

                        // these are allocated by the library
                        __device__ float *alpha_iso;
                        __device__ float *alpha_union;

                        __device__ inline float eval(const vec3<float>& r_ij,
                            unsigned int type_i,
                            const quat<float>& q_i,
                            float d_i,
                            float charge_i,
                            unsigned int type_j,
                            const quat<float>& q_j,
                            float d_j,
                            float charge_j)
                            {
                        """
        cpp_function += code
        cpp_function += """
                            }
                        """
        # Compile on C++ side
        return cpp_function

class UserPatch(PatchCompute):
    R''' Define an arbitrary patch energy.

    Args:
        r_cut (float): Particle center to center distance cutoff beyond which all pair interactions are assumed 0.
        code (str): C++ code defining the costum pair interactions between particles.
        llvm_ir_fname (str): File name of the llvm IR file to load.
        clang_exec (str): The Clang executable to use
        array_size (int): Size of array with adjustable elements. (added in version 2.8)

    Patch energies define energetic interactions between pairs of shapes in :py:mod:`hpmc <hoomd.hpmc>` integrators.
    Shapes within a cutoff distance of *r_cut* are potentially interacting and the energy of interaction is a function
    the type and orientation of the particles and the vector pointing from the *i* particle to the *j* particle center.

    The :py:class:`UserPatch` patch energy takes C++ code, JIT compiles it at run time and executes the code natively
    in the MC loop with full performance. It enables researchers to quickly and easily implement custom energetic
    interactions without the need to modify and recompile HOOMD. Additionally, :py:class:`UserPatch` provides a mechanism,
    through the `alpha_iso` attribute (numpy array), to adjust user defined potential parameters without the need
    to recompile the patch energy code. These arrays are **read-only** during function evaluation.

    Attributes:
        r_cut (float): Particle center to center distance cutoff beyond which all pair interactions are assumed 0.
        log_only (bool): Enable patch interaction for logging purposes only.
        array_size (int): Size of array with adjustable elements. (added in version 2.8)
        energy (float): Total interaction energy of the system in the current state.
        alpha_iso (numpy.ndarray, float): Length array_size numpy array containing dynamically adjustable elements
                                          defined by the user (added in version 2.8).

    .. rubric:: C++ code

    Supply C++ code to the *code* argument and :py:class:`UserPatch` will compile the code and call it to evaluate
    patch energies. Compilation assumes that a recent ``clang`` installation is on your PATH. This is convenient
    when the energy evaluation is simple or needs to be modified in python. More complex code (i.e. code that
    requires auxiliary functions or initialization of static data arrays) should be compiled outside of HOOMD
    and provided via the *llvm_ir_file* input (see below).

    The text provided in *code* is the body of a function with the following signature:

    .. code::

        float eval(const vec3<float>& r_ij,
                   unsigned int type_i,
                   const quat<float>& q_i,
                   float d_i,
                   float charge_i,
                   unsigned int type_j,
                   const quat<float>& q_j,
                   float d_j,
                   float charge_j)

    * ``vec3`` and ``quat`` are defined in HOOMDMath.h.
    * *r_ij* is a vector pointing from the center of particle *i* to the center of particle *j*.
    * *type_i* is the integer type of particle *i*
    * *q_i* is the quaternion orientation of particle *i*
    * *d_i* is the diameter of particle *i*
    * *charge_i* is the charge of particle *i*
    * *type_j* is the integer type of particle *j*
    * *q_j* is the quaternion orientation of particle *j*
    * *d_j* is the diameter of particle *j*
    * *charge_j* is the charge of particle *j*
    * Your code *must* return a value.
    * When \|r_ij\| is greater than *r_cut*, the energy *must* be 0. This *r_cut* is applied between
      the centers of the two particles: compute it accordingly based on the maximum range of the anisotropic
      interaction that you implement.

    Examples:

    Static potential parameters

    .. code-block:: python

        square_well = """float rsq = dot(r_ij, r_ij);
                            if (rsq < 1.21f)
                                return -1.0f;
                            else
                                return 0.0f;
                      """
        patch = hoomd.jit.patch.UserPatch(r_cut=1.1, code=square_well)
        sim.operations += patch
        sim.run(1000)

    Dynamic potential parameters

    .. code-block:: python

        square_well = """float rsq = dot(r_ij, r_ij);
                         float r_cut = alpha_iso[0];
                            if (rsq < r_cut*r_cut)
                                return alpha_iso[1];
                            else
                                return 0.0f;
                      """
        patch = hoomd.jit.patch.UserPatch(r_cut=1.1, array_size=2, code=square_well)
        patch.alpha_iso[:] = [1.1, 1.5] # [rcut, epsilon]
        sim.operations += patch
        sim.run(1000)
        patch.alpha_iso[1] = 2.0
        sim.run(1000)

    .. rubric:: LLVM IR code

    You can compile outside of HOOMD and provide a direct link
    to the LLVM IR file in *llvm_ir_file*. A compatible file contains an extern "C" eval function with this signature:

    .. code::

        float eval(const vec3<float>& r_ij,
                   unsigned int type_i,
                   const quat<float>& q_i,
                   float d_i,
                   float charge_i,
                   unsigned int type_j,
                   const quat<float>& q_j,
                   float d_j,
                   float charge_j)

    ``vec3`` and ``quat`` are defined in HOOMDMath.h.

    Compile the file with clang: ``clang -O3 --std=c++14 -DHOOMD_LLVMJIT_BUILD -I /path/to/hoomd/include -S -emit-llvm code.cc`` to produce
    the LLVM IR in ``code.ll``.

    .. versionadded:: 2.3
    '''
    def __init__(self, r_cut, array_size=1, log_only=False,
                 code=None, llvm_ir_file=None, clang_exec=None):
        super().__init__(r_cut=r_cut, array_size=array_size, log_only=log_only,
                         code=code, llvm_ir_file=llvm_ir_file, clang_exec=clang_exec)

    def _attach(self):
        integrator = self._simulation.operations.integrator
        if not isinstance(integrator, integrate.HPMCIntegrator):
            raise RuntimeError("The integrator must be a HPMC integrator.")

        if not integrator._attached:
            raise RuntimeError("Integrator is not attached yet.")

        clang = self._clang_exec if self._clang_exec is not None else 'clang'
        # compile code if provided
        if self._code is not None:
            llvm_ir = self._compile_user(self._code, clang)
        # fall back to LLVM IR file in case code is not provided
        elif self._llvm_ir_file is None:
            # IR is a text file
            with open(self._llvm_ir_file,'r') as f:
                llvm_ir = f.read()
        else:
            raise RuntimeError("")

        cpp_exec_conf = self._simulation.device._cpp_exec_conf
        if (isinstance(self._simulation.device, hoomd.device.GPU)):
            self._setup_gpu_code_path()
            gpu_code = self._wrap_gpu_code(self._code)
            self._cpp_obj = _jit.PatchEnergyJITGPU(cpp_exec_conf, llvm_ir, self.r_cut, self.array_size,
                gpu_code, "hpmc::gpu::kernel::hpmc_narrow_phase_patch", self._options, self._cuda_devrt_library_path, self._max_arch);
        else:
            self._cpp_obj = _jit.PatchEnergyJIT(cpp_exec_conf, llvm_ir, self.r_cut, self.array_size)
        # Set the C++ mirror array with the cached values
        # and override the python array
        self._cpp_obj.alpha_iso[:] = self.alpha_iso[:]
        self.alpha_iso = self._cpp_obj.alpha_iso
        # attach patch object to the integrator
        self._simulation.operations.integrator._cpp_obj.setPatchEnergy(self._cpp_obj)
        super()._attach()

class UserUnionPatch(PatchCompute):
    R''' Define an arbitrary patch energy on a union of particles

    Args:
        r_cut_union (float): Constituent particle center to center distance cutoff beyond which all pair interactions are assumed 0.
        r_cut (float, **optional**): Cut-off for isotropic interaction between centers of union particles
        code_union (str): C++ code to compile
        code (str, **optional**): C++ code for isotropic part
        llvm_ir_fname_union (str): File name of the llvm IR file to load.
        llvm_ir_fname (str, **optional**): File name of the llvm IR file to load for isotropic interaction
        array_size_union (int): Size of array with adjustable elements. (added in version 2.8)
        array_size (int): Size of array with adjustable elements for the isotropic part. (added in version 2.8)

    Attributes:
        positions (`TypeParameter` [``particle type``, `list` [`tuple` [`float`, `float`, `float`]]])
            The positions of the constituent particles
        orientations (`TypeParameter` [``particle type``, `list` [`tuple` [`float`, `float`, `float, `float`]]])
            The orientations of the constituent particles (list of four-vectors)
        diameters (`TypeParameter` [``particle type``, `list` [`float`]])
            The diameters of the constituent particles (list of floats)
        charges (`TypeParameter` [``particle type``, `list` [`float`]])
            The charges of the constituent particles (list of floats)
        typeids (`TypeParameter` [``particle type``, `list` [`float`]])
            The charges of the constituent particles (list of floats)
        leaf_capacity (`int`, **default:** 4) : The number of particles in a leaf of the internal tree data structure
        alpha_union (numpy.ndarray, float): Length array_size_union numpy array containing dynamically adjustable elements
                                            defined by the user for unions of shapes (added in version 2.8)

    Example:

    .. code-block:: python

        square_well = """float rsq = dot(r_ij, r_ij);
                            if (rsq < 1.21f)
                                return -1.0f;
                            else
                                return 0.0f;
                      """
        patch = hoomd.jit.patch.UserUnionPatch(r_cut_union=1.1, code_union=square_well)
        patch.positions['A'] = [(0,0,-5.),(0,0,.5)]
        patch.typeids['A'] =[0,0]

    Example with added isotropic interactions:

    .. code-block:: python

        # square well attraction on constituent spheres
        square_well = """float rsq = dot(r_ij, r_ij);
                              float r_cut = alpha_union[0];
                              if (rsq < r_cut*r_cut)
                                  return alpha_union[1];
                              else
                                  return 0.0f;
                           """

        # soft repulsion between centers of unions
        soft_repulsion = """float rsq = dot(r_ij, r_ij);
                                  float r_cut = alpha_iso[0];
                                  if (rsq < r_cut*r_cut)
                                    return alpha_iso[1];
                                  else
                                    return 0.0f;
                         """

        patch = hoomd.jit.patch.UserUnionPatch(r_cut_union=2.5, code_union=square_well, array_size_union=2, \
                                               r_cut=5, code=soft_repulsion, array_size=2)
        patch.positions['A'] = [(0,0,-5.),(0,0,.5)]
        patch.typeids['A'] = [0,0]
        # [r_cut, epsilon]
        patch.alpha_iso[:] = [2.5, 1.3];
        patch.alpha_union[:] = [2.5, -1.7];

    .. versionadded:: 2.3
    '''
    def __init__(self, r_cut_union, array_size_union=1, code_union=None, llvm_ir_file_union=None,
                 r_cut=None, array_size=1, log_only=False, code=None, llvm_ir_file=None, clang_exec=None):

        r_cut = r_cut if r_cut is not None else -1.0

        # initialize base class
        super().__init__(r_cut, array_size, log_only, code, llvm_ir_file, clang_exec)

        # add union specific params
        param_dict = ParameterDict(r_cut_union = r_cut_union,
                                   array_size_union = array_size_union,
                                   leaf_capacity = int(4))
        self._param_dict.update(param_dict)

        # add union specific per-type parameters
        typeparam_positions = TypeParameter('positions',
                                            type_kind='particle_types',
                                            param_dict=TypeParameterDict([],
                                            len_keys=1))

        typeparam_orientations = TypeParameter('orientations',
                                               type_kind='particle_types',
                                               param_dict=TypeParameterDict([],
                                               len_keys=1))

        typeparam_diameters = TypeParameter('diameters',
                                            type_kind='particle_types',
                                            param_dict=TypeParameterDict([],
                                            len_keys=1))

        typeparam_charges = TypeParameter('charges',
                                          type_kind='particle_types',
                                          param_dict=TypeParameterDict([],
                                          len_keys=1))

        typeparam_typeids = TypeParameter('typeids',
                                          type_kind='particle_types',
                                          param_dict=TypeParameterDict([],
                                          len_keys=1))

        self._extend_typeparam([typeparam_positions, typeparam_orientations,
                                typeparam_diameters, typeparam_charges,
                                typeparam_typeids])

        # these only exist on python
        self._code_union = code_union
        self._llvm_ir_file_union = llvm_ir_file_union
        self.alpha_union = np.zeros(array_size_union)

    def _attach(self):
        integrator = self._simulation.operations.integrator
        if not isinstance(integrator, integrate.HPMCIntegrator):
            raise RuntimeError("The integrator must be a HPMC integrator.")

        if not integrator._attached:
            raise RuntimeError("Integrator is not attached yet.")

        clang = self._clang_exec if self._clang_exec is not None else 'clang'
        # compile code if provided
        if self._code_union is not None:
            llvm_ir_union = self._compile_user(self._code_union, clang)
        # fall back to LLVM IR file in case code is not provided
        elif self._llvm_ir_file_union is None:
            # IR is a text file
            with open(self._llvm_ir_file_union,'r') as f:
                llvm_ir_union = f.read()
        else:
            raise RuntimeError("")

        if self._code is not None:
            llvm_ir = self._compile_user(self._code, clang)
        elif self._llvm_ir_file is not None:
            # IR is a text file
            with open(self._llvm_ir_file,'r') as f:
                llvm_ir = f.read()
        else:
            # provide a dummy function
            llvm_ir = self._compile_user('return 0.0;', clang)

        cpp_exec_conf = self._simulation.device._cpp_exec_conf
        if (isinstance(self._simulation.device, hoomd.device.GPU)):
            self._setup_gpu_code_path()
            # use union evaluator
            self._options += ["-DUNION_EVAL"]
            gpu_code = self._wrap_gpu_code(self._code)
            self._cpp_obj = _jit.PatchEnergyJITUnionGPU(self._simulation.state._cpp_sys_def, cpp_exec_conf,
                llvm_ir, self.r_cut, self.array_size, llvm_ir_union, self.r_cut_union,  self.array_size_union,
                gpu_code, "hpmc::gpu::kernel::hpmc_narrow_phase_patch", self._options, self._cuda_devrt_library_path, self._max_arch);
        else:
            self._cpp_obj = _jit.PatchEnergyJITUnion(self._simulation.state._cpp_sys_def, cpp_exec_conf,
                llvm_ir, self.r_cut, self.array_size, llvm_ir_union, self.r_cut_union,  self.array_size_union)

        # Set the C++ mirror array with the cached values
        # and override the python array
        self._cpp_obj.alpha_iso[:] = self.alpha_iso[:]
        self._cpp_obj.alpha_union[:] = self.alpha_union[:]
        self.alpha_iso = self._cpp_obj.alpha_iso
        self.alpha_union = self._cpp_obj.alpha_union
        # attach patch object to the integrator
        self._simulation.operations.integrator._cpp_obj.setPatchEnergy(self._cpp_obj)
        super()._attach()

    @property
    def code_union(self):
        return self._code_union

    @code_union.setter
    def code_union(self, code):
        if self._attached():
            raise AttributeError("This attribute can only be set when the patch is not attached.")
        else:
            self._code_union = code_union

    @property
    def llvm_ir_file_union(self):
        return self._llvm_ir_file

    @llvm_ir_file_union.setter
    def llvm_ir_file_union(self, llvm_ir):
        if self._attached():
            raise AttributeError("This attribute can only be set when the patch is not attached.")
        else:
            self._llvm_ir_file_union = llvm_ir
