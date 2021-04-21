# Copyright (c) 2009-2019 The Regents of the University of Michigan
# This file is part of the HOOMD-blue project, released under the BSD 3-Clause License.

# Maintainer: joaander / All Developers are free to add commands for new features

import hoomd
from hoomd.md import _md
from hoomd import _hoomd
from hoomd.operation import _HOOMDBaseObject
from hoomd.data.parameterdicts import ParameterDict, TypeParameterDict
from hoomd.data.typeconverter import OnlyIf, to_type_converter
from collections.abc import Sequence

# A manifold in hoomd reflects a Manifold in c++. It is responsible to define the manifold 
# used for RATTLE integrators and the active force constraints.
class Manifold(_HOOMDBaseObject):
    r"""Base class manifold object.

    Manifold defines a positional constraint to a given set of particles. A manifold can 
    be applied to a RATTLE method and/or the active force class. The degrees of freedom removed from 
    the system by constraints are correctly taken into account, i.e. when computing temperature 
    for thermostatting and/or logging.

    All manifolds are described by implicit functions.

    Note:
        Users should not instantiate :py:class:`Manifold` directly, but should instead instantiate
        one of its subclasses defining a specific manifold geometry.

    Warning:
        Only one manifold can be applied to the methods/active forces.

    """
    def _attach(self):
        self._apply_param_dict()
        self._apply_typeparam_dict(self._cpp_obj, self._simulation)

    @staticmethod
    def _preprocess_unitcell(value):
        if isinstance(value, Sequence):
            if len(value) != 3:
                raise ValueError(
                    "Expected a single int or a sequence of three ints.")
            return tuple(value)
        else:
            return (value,value,value)

    def __eq__(self, other):
        if not isinstance(other, type(self)):
            return NotImplemented
        return all(getattr(self, attr) == getattr(other, attr) for attr in self._param_dict)

    def _setattr_param(self, attr, value):
        raise AttributeError("Manilfolds are immutable and {} cannot be set "
                            "after initialization".format(attr))


class Cylinder(Manifold):
    R""" Cylinder manifold.

    Args:
        r (`float`): radius of the cylinder constraint (in distance units).
        P (`tuple` [`float`, `float`, `float`]): point defining position of the cylinder axis (default origin).

    :py:class:`Cylinder` specifies that a cylindric manifold is defined as
    a constraint.

    Note:
        The cylinder axis is parallel to the z-direction.

    .. rubric:: Implicit function

    .. math::
        F(x,y,z) = x^{2} + y^{2} - r^{2}

    Example::

        cylinder1 = manifold.Cylinder(r=10)
        cylinder2 = manifold.Cylinder(r=5,P=(1,1,1))
    """
    def __init__(self,r, P=(0,0,0) ):
        # initialize the base class
        param_dict = ParameterDict(
            r=float(r),
            P=(float, float,float),
        )
        param_dict['P']= P

        self._param_dict.update(param_dict)

    def __eq__(self, other):
        """Return a Boolean indicating whether the two manifolds are equivalent.
        """
        return (
            isinstance(other, Cylinder)
            and self.r == other.r
            and self.P == other.P
        )

    def _attach(self):
        self._cpp_obj = _md.ManifoldCylinder(self.r, _hoomd.make_scalar3( self.P[0], self.P[1], self.P[2]) );

        super()._attach()

class Diamond(Manifold):

    R""" Triply periodic diamond manifold.

    Args:
        N (`tuple` [`int`, `int`, `int`] or `int`): number of unit cells in all 3 directions.
            :math:`[N_x, N_y, N_z]`. In case number of unit cells u in all
            direction the same (:math:`[u, u, u]`), use ``N = u``.
        epsilon (`float`): defines CMC companion of the Diamond surface (default 0) 

    :py:class:`Diamond` specifies a periodic diamond surface as a constraint.
    The diamond (or Schwarz D) belongs to the family of triply periodic minimal surfaces.

    
    For the diamond surface, see:
    * `A. H. Schoen 1970  <https://ntrs.nasa.gov/citations/19700020472>`_
    * `P. J. F. Gandy et. al. 1999  <https://doi.org/10.1016/S0009-2614(99)01000-3>`_
    * `H. G. von Schnering and R. Nesper 1991  <https://doi.org/10.1007/BF01313411>`_
    
    .. rubric:: Implicit function
    
    .. math::
        F(x,y,z) = \cos{\frac{2 \pi}{L_x} x}\cdot\cos{\frac{2 \pi}{L_y} y}\cdot\cos{\frac{2 \pi}{L_z} z} - \sin{\frac{2 \pi}{L_x} x}\cdot\sin{\frac{2 \pi}{L_y} y}\cdot\sin{\frac{2 \pi}{L_z} z} - \epsilon

    is the nodal approximation of the diamond surface where :math:`[L_x,L_y,L_z]` 
    is the periodicity length in the x, y and z direction. The periodicity length 
    L is defined by the current box size B and the number of unit cells N. 
    :math:`L=\frac{B}{N}`
    
    Example::
    
        diamond1 = manifold.Diamond(N=1)
        diamond2 = manifold.Diamond(N=(1,2,2))
    """
    def __init__(self,N,epsilon=0):

        # store metadata
        param_dict = ParameterDict(
            N=OnlyIf(to_type_converter((int,)*3), preprocess=self._preprocess_unitcell),
            epsilon=float(epsilon),
        )
        param_dict['N'] = N
        self._param_dict.update(param_dict)

    def _attach(self):
        self._cpp_obj = _md.ManifoldDiamond( _hoomd.make_int3(self.N[0], self.N[1], self.N[2]), self.epsilon );

        super()._attach()

class Ellipsoid(Manifold):
    """ Ellipsoid manifold.

    Args:
        a (`float`): length of the a-axis of the ellipsoidal constraint (in distance units).
        b (`float`): length of the b-axis of the ellipsoidal constraint (in distance units).
        c (`float`): length of the c-axis of the ellipsoidal constraint (in distance units).
        P (`tuple` [`float`, `float`, `float`]): center of the ellipsoidal constraint (default origin).
    
    :py:class:`Ellipsoid` specifies that a ellipsoidal manifold is defined as a constraint. 
    
    .. rubric:: Implicit function
    
    .. math::
        F(x,y,z) = \\frac{x^{2}}{a^{2}} + \\frac{y^{2}}{b^{2}} + \\frac{z^{2}}{c^{2}} - 1      
    
    Example::
    
        ellipsoid1 = manifold.Ellipsoid(a=10,b=5,c=5)
        ellipsoid2 = manifold.Ellipsoid(a=5,b=10,c=10,P=(1,0.5,1))
    """
    def __init__(self,a,b,c, P=(0,0,0) ):
        # store metadata
        param_dict = ParameterDict(
            a=float(a),
            b=float(b),
            c=float(c),
            P=(float, float,float),
        )
        param_dict['P'] = P

        self._param_dict.update(param_dict)

    def _attach(self):
        self._cpp_obj = _md.ManifoldEllipsoid(self.a, self.b, self.c,  _hoomd.make_scalar3( self.P[0], self.P[1], self.P[2]) );

        super()._attach()

class Gyroid(Manifold):

    R""" Triply periodic gyroid manifold.

    Args:
        N (`tuple` [`int`, `int`, `int`] or `int`): number of unit cells in all 3 directions.
            :math:`[N_x, N_y, N_z]`. In case number of unit cells u in all
            direction the same (:math:`[u, u, u]`), use ``N = u``.
        epsilon (`float`): defines CMC companion of the Gyroid surface (default 0) 
        
    :py:class:`Gyroid` specifies a periodic gyroid surface as a constraint.
    The gyroid belongs to the family of triply periodic minimal surfaces.

    For the gyroid surface, see:
    
    * `A. H. Schoen 1970  <https://ntrs.nasa.gov/citations/19700020472>`_
    * `P. J.F. Gandy et. al. 2000  <https://doi.org/10.1016/S0009-2614(00)00373-0>`_
    * `H. G. von Schnering and R. Nesper 1991  <https://doi.org/10.1007/BF01313411>`_
    
    .. rubric:: Implicit function
    
    .. math::
        F(x,y,z) = \sin{\frac{2 \pi}{L_x} x}\cdot\cos{\frac{2 \pi}{L_y} y} + \sin{\frac{2 \pi}{L_y} y}\cdot\cos{\frac{2 \pi}{L_z} z} + \sin{\frac{2 \pi}{L_z} z}\cdot\cos{\frac{2 \pi}{L_x} x} - \epsilon
    
    is the nodal approximation of the diamond surface where :math:`[L_x,L_y,L_z]` 
    is the periodicity length in the x, y and z direction. The periodicity length 
    L is defined by the current box size B and the number of unit cells N. 
    :math:`L=\frac{B}{N}`
    
    Example::
    
        gyroid1 = manifold.Gyroid(N=1)
        gyroid2 = manifold.Gyroid(N=(1,2,2))
    """
    def __init__(self,N,epsilon=0):

        # initialize the base class
        super().__init__();
        # store metadata
        param_dict = ParameterDict(
            N=OnlyIf(to_type_converter((int,)*3), preprocess=self._preprocess_unitcell),
            epsilon=float(epsilon),
        )
        param_dict['N'] = N
        self._param_dict.update(param_dict)

    def _attach(self):
        self._cpp_obj = _md.ManifoldGyroid( _hoomd.make_int3(self.N[0], self.N[1], self.N[2]), self.epsilon );

        super()._attach()


class Plane(Manifold):
    R""" Plane manifold.
    
    Args:
        shift (`float`): z-shift of the xy-plane (in distance units).

    :py:class:`Plane` specifies that a xy-plane manifold is defined as 
    a constraint. 

    .. rubric:: Implicit function

    .. math::
        F(x,y,z) = z - \textrm{shift}

    Example::

        plane1 = manifold.Plane()
        plane2 = manifold.Plane(shift=0.8)
    """
    def __init__(self,shift=0):
        param_dict = ParameterDict(
            shift=float(shift),
        )

        self._param_dict.update(param_dict)

    def __eq__(self, other):
        """Return a Boolean indicating whether the two manifolds are equivalent.
        """
        return (
            isinstance(other, Plane)
            and self.shift == other.shift
        )

    def _attach(self):
        self._cpp_obj = _md.ManifoldXYPlane(self.shift);

        super()._attach()

class Primitive(Manifold):

    R""" Triply periodic primitive manifold.

    Args:
        N (`tuple` [`int`, `int`, `int`] or `int`): number of unit cells in all 3 directions.
            :math:`[N_x, N_y, N_z]`. In case number of unit cells u in all
            direction the same (:math:`[u, u, u]`), use ``N = u``.
        epsilon (`float`): defines CMC companion of the Primitive surface (default 0) 
        
    :py:class:`Primitive` specifies a periodic primitive surface as a constraint.
    The primitive (or Schwarz P) belongs to the family of triply periodic minimal surfaces.

    For the primitive surface, see:
    
    * `A. H. Schoen 1970  <https://ntrs.nasa.gov/citations/19700020472>`_
    * `P. J. F. Gandy et. al. 2000  <https://doi.org/10.1016/S0009-2614(00)00453-X>`_
    * `H. G. von Schnering and R. Nesper 1991  <https://doi.org/10.1007/BF01313411>`_
    
    .. rubric:: Implicit function
    
    .. math::
        F(x,y,z) = \cos{\frac{2 \pi}{L_x} x} + \cos{\frac{2 \pi}{L_y} y} + \cos{\frac{2 \pi}{L_z} z} - \epsilon

    is the nodal approximation of the diamond surface where :math:`[L_x,L_y,L_z]` 
    is the periodicity length in the x, y and z direction. The periodicity length 
    L is defined by the current box size B and the number of unit cells N. 
    :math:`L=\frac{B}{N}`
    
    Example::
    
        primitive1 = manifold.Primitive(N=1)
        primitive2 = manifold.Primitive(N=(1,2,2))
    """
    def __init__(self,N,epsilon=0):

        # store metadata
        param_dict = ParameterDict(
            N=OnlyIf(to_type_converter((int,)*3), preprocess=self._preprocess_unitcell),
            epsilon=float(epsilon),
        )
        param_dict['N'] = N
        self._param_dict.update(param_dict)

    def _attach(self):
        self._cpp_obj = _md.ManifoldPrimitive( _hoomd.make_int3(self.N[0], self.N[1], self.N[2]), self.epsilon );

        super()._attach()

class Sphere(Manifold):
    """ Sphere manifold.

    Args:
        r (`float`): raduis of the a-axis of the spherical constraint (in distance units).
        P (`tuple` [`float`, `float`, `float`] ): center of the spherical constraint (default origin).

    :py:class:`Sphere` specifies that a spherical manifold is defined as 
    a constraint. 

    .. rubric:: Implicit function

    .. math::
        F(x,y,z) = x^{2} + y^{2} + z^{2} - r^{2}

    Example::

        sphere1 = manifold.Sphere(r=10)
        sphere2 = manifold.Sphere(r=5,P=(1,0,1.5))
    """
    def __init__(self,r, P=(0,0,0) ):
        # initialize the base class
        super().__init__();
        param_dict = ParameterDict(
            r=float(r),
            P=(float, float,float),
        )
        param_dict['P'] = P

        self._param_dict.update(param_dict)

    def _attach(self):
        self._cpp_obj = _md.ManifoldSphere(self.r, _hoomd.make_scalar3( self.P[0], self.P[1], self.P[2]) );

        super()._attach()

