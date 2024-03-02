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

PairPotentialAngularStep::PairPotentialAngularStep(
    std::shared_ptr<SystemDefinition> sysdef,
    std::shared_ptr<PairPotential> isotropic_potential)
    : PairPotential(sysdef), m_isotropic_potential(isotropic_potential)
    {
    unsigned int ntypes = m_sysdef->getParticleData()->getNTypes();
    m_directors.resize(ntypes);
    m_deltas.resize(ntypes);

    if (!m_isotropic_potential)
        {
        throw std::runtime_error("Could not pass in the isotropic potential.");
        }
    }

void PairPotentialAngularStep::setPatch(std::string particle_type, pybind11::object v)
    {
    unsigned int particle_type_id = m_sysdef->getParticleData()->getTypeByName(particle_type);

    if (v.is_none())
        {
        m_directors[particle_type_id].clear();
        m_deltas.clear();
        return;
        }
    pybind11::list directors = v["directors"];
    pybind11::list deltas = v["deltas"];

    auto N = pybind11::len(directors);

    if (pybind11::len(deltas) != N)
        {
        throw std::runtime_error("the length of the delta list should match the length"
                                 "of the director list.");
        }

    m_directors[particle_type_id].resize(N);
    m_deltas.resize(N);

    for (unsigned int i = 0; i < N; i++)
        {
        pybind11::tuple director_python = directors[i];
        if (pybind11::len(director_python) != 3)
            {
            throw std::length_error("director must be a list of 3-tuples.");
            }
        m_directors[particle_type_id][i] = vec3<LongReal>(director_python[0].cast<LongReal>(),
                                                          director_python[1].cast<LongReal>(),
                                                          director_python[2].cast<LongReal>());
        // TO DO: normalize m_director to be unit length

        pybind11::handle delta_python = deltas[i];
        m_deltas[particle_type_id][i] = delta_python.cast<LongReal>();
        }
    }

pybind11::object PairPotentialAngularStep::getPatch(std::string particle_type)
    {
    unsigned int particle_type_id = m_sysdef->getParticleData()->getTypeByName(particle_type);
    size_t N = m_directors[particle_type_id].size();

    if (N == 0)
        {
        return pybind11::none();
        }

    pybind11::list directors;
    pybind11::list deltas;

    for (unsigned int i = 0; i < N; i++)
        {
        directors.append(pybind11::make_tuple(m_directors[particle_type_id][i].x,
                                              m_directors[particle_type_id][i].y,
                                              m_directors[particle_type_id][i].z));
        deltas.append(m_deltas[particle_type_id][i]);
        }

    pybind11::dict v;
    v["directors"] = directors;
    v["deltas"] = deltas;
    return v;
    }

// protected
bool PairPotentialAngularStep::maskingFunction(LongReal r_squared,
                                               const vec3<LongReal>& r_ij,
                                               const unsigned int type_i,
                                               const quat<LongReal>& q_i,
                                               const unsigned int type_j,
                                               const quat<LongReal>& q_j) const
    {
    vec3<LongReal> rhat_ij = r_ij / fast::sqrt(r_squared);

    for (int m = 0; m < m_directors[type_i].size(); m++)
        {
        LongReal cos_delta_m = cos(m_deltas[type_i][m]);
        vec3<LongReal> ehat_m = rotate(q_i, m_directors[type_i][m]);

        for (int n = 0; n < m_directors[type_j].size(); n++)
            {
            LongReal cos_delta_n = cos(m_deltas[type_j][n]);
            vec3<LongReal> ehat_n = rotate(q_j, m_directors[type_j][n]);

            if (dot(ehat_m, rhat_ij) >= cos_delta_m && dot(ehat_n, -rhat_ij) >= cos_delta_n)
                {
                return true;
                }
            }
        }
    return false;
    }

LongReal PairPotentialAngularStep::energy(const LongReal r_squared,
                                          const vec3<LongReal>& r_ij,
                                          const unsigned int type_i,
                                          const quat<LongReal>& q_i,
                                          const LongReal charge_i,
                                          const unsigned int type_j,
                                          const quat<LongReal>& q_j,
                                          const LongReal charge_j) const
    {
    if (maskingFunction(r_squared, r_ij, type_i, q_i, type_j, q_j))
        {
        return m_isotropic_potential
            ->energy(r_squared, r_ij, type_i, q_i, charge_i, type_j, q_j, charge_j);
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
        .def("getPatch", &PairPotentialAngularStep::getPatch);
    }
    } // end namespace detail

    } // end namespace hpmc
    } // end namespace hoomd
