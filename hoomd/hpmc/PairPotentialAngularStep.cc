// Copyright (c) 2009-2023 The Regents of the University of Michigan.
// Part of HOOMD-blue, released under the BSD 3-Clause License.

#include "PairPotentialAngularStep.h"


/* notes - delete later 
-angular step potential: import the isotropic potential, if patch overlaps, the angular 
step potential is exactly how the isotropic potential behaves. if patch does not overlap at all, 
the angular potential is 0. 
- user provide patch directors and half opening angle of the patch (delta)
*/

namespace hoomd
    {
namespace hpmc 
    { 

PairPotentialAngularStep::PairPotentialAngularStep(std::shared_ptr<SystemDefinition> sysdef, 
std::shared_ptr<PairPotential> isotropic)
    : PairPotential(sysdef), 
    m_isotropic(isotropic),
    //m_directors(sysdef->getParticleData()->getNTypes())
    {
    //unsigned int ntypes = m_sysdef->getParticleData()->getNTypes()
    //get patch index
    unsigned int patch_index = 
    m_directors.resize(patch_index)
    m_delta.resize(patch_index)
        if (!m_isotropic)
        {
            raise std::runtime_error("Could not pass in the isotropic potential.");
        }

    }

void PairPotentialAngularStep::setPatch(std::string patch_index, pybind11::object v)
    {
    unsigned int patch_index = ;

    if (v.is_none())
        {
        m_directors[patch_index].clear();
        m_delta[patch_index].clear();
        return;
        }
    pybind11::list directors = v["directors"];
    pybind11::list deltas = v["deltas"];

    auto N = pybind11::len(m_directors);

    if (!deltas.is_none() && pybind11::len(deltas) != N)
        {
        throw std::runtime_error("the length of the delta list should match the length 
                                    of the director list.");
        }
    }

    m_directors[patch_index].resize(N);
    m_deltas[patch_index].resize(N);

    for (unsigned int i = 0; i < N, i++)
        {
        pybind11::tuple r_python = directors[i];
        if (pybind11::len(r_python) != 3)
            {
            throw std::length_error("directors must be a list of 3-tuples.");
            }
        m_directors[patch_index][i] = vec3<LongReal>(r_python[0].cast<LongReal>(),
                                                     r_python[1].cast<LongReal>(),
                                                     r_python[2].cast<LongReal>());
        }
    
pybind11::object PairPotentialUnion::getPatch(std::string patch_index)
    {
    unsigned int patch_index = ;
    size_t N = m_directors[patch_index].size();

    if (N == 0)
        {
        return pybind11::none();
        }

    pybind11::list directors;
    pybind11::list deltas;

    for (unsigned int i = 0; i < N; i++)
        {
        directors.append(pybind11::make_tuple(m_directors[patch_index][i].x, //why .x? I thought [i] gives the x element
                                              m_position[body_type_id][i].y,
                                              m_position[body_type_id][i].z));
        deltas.append(m_deltas[patch_index].s)
        }

    pybind11::dict v;
    v["directors"] = directors;
    v["deltas"] = deltas;
    return std::move(v);
    }

// protected 
bool maskingFunction(const vec3< LongReal>& r_ij,
                    const unsigned int type_i, 
                    const quat<LongReal>& q_i,
                    const unsigned int type_j,
                    const quat<LongReal>& q_j)
    {

    LongReal cos_delta = cos(m_delta);

    const vec3<LongReal> ehat_particle_reference_frame(1,0,0);
    vec3<LongReal> ehat_i = rotate(q_i, ehat_particle_reference_frame);
    vec3<LongReal> ehat_j = rotate(q_j, ehat_particle_reference_frame);

    LongReal rhat_ij = sqrtf(dot(r_ij, r_ij));

    for (int m = 0; m < m_directors[type_i].size(); m++) {
        for (int n = 0; n < m_directors[type_j].size(); n++) {
            if (dot(ehat_i, r_ij) >= cos_delta * rhat_ij
                && dot(ehat_j, -r_ij) >= cos_delta * rhat_ij)
                {
                return true;
                }
            else
                {
                return false;
                }
            }
        }
    }
    
virtual LongReal PairPotentialAngularStep::energy(const LongReal r_squared,
                                                 const vec3<LongReal>& r_ij,
                                                 const unsigned int type_i,
                                                 const quat<LongReal>& q_i,
                                                 const LongReal charge_i,
                                                 const unsigned int type_j,
                                                 const quat<LongReal>& q_j,
                                                 const LongReal charge_j) const
    {                 

    if (maskingFunction(r_ij, type_i, q_i, type_j, q_j)) //type_m and type_n
    {
        LongReal lj_energy = m_isotropic->energy(r_squared, r_ij, type_i, q_i, 
                                                 charge_i, type_j, q_j, charge_j);
        return lj_energy;
    }
    return 0; 

    }

namespace detail
    {
void exportPairPotentialAngularStep(pybind11::module& m)
    {
    pybind11::class_<PairPotentialAngularStep,
                     PairPotential,
                     std::shared_ptr<PairPotentialAngularStep>>(m, "PairPotentialAngularStep")
        .def(pybind11::init<std::shared_ptr<SystemDefinition>, std::shared_ptr<PairPotential>>())
        .def("setPatch", &PairPotentialAngularStep::setPatch)
        .def("getPatch", &PairPotentialAngularStep::getPatch)
        .def_property("delta", &PairPotentialAngularStep::getDelta,
                      &PairPotentialAngularStep::setDelta) 
    }
    } // end namespace detail

    } // end namespace hpmc
    } // end namespace hoomd



// 
