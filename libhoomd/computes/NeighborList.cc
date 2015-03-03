/*
Highly Optimized Object-oriented Many-particle Dynamics -- Blue Edition
(HOOMD-blue) Open Source Software License Copyright 2009-2014 The Regents of
the University of Michigan All rights reserved.

HOOMD-blue may contain modifications ("Contributions") provided, and to which
copyright is held, by various Contributors who have granted The Regents of the
University of Michigan the right to modify and/or distribute such Contributions.

You may redistribute, use, and create derivate works of HOOMD-blue, in source
and binary forms, provided you abide by the following conditions:

* Redistributions of source code must retain the above copyright notice, this
list of conditions, and the following disclaimer both in the code and
prominently in any materials provided with the distribution.

* Redistributions in binary form must reproduce the above copyright notice, this
list of conditions, and the following disclaimer in the documentation and/or
other materials provided with the distribution.

* All publications and presentations based on HOOMD-blue, including any reports
or published results obtained, in whole or in part, with HOOMD-blue, will
acknowledge its use according to the terms posted at the time of submission on:
http://codeblue.umich.edu/hoomd-blue/citations.html

* Any electronic documents citing HOOMD-Blue will link to the HOOMD-Blue website:
http://codeblue.umich.edu/hoomd-blue/

* Apart from the above required attributions, neither the name of the copyright
holder nor the names of HOOMD-blue's contributors may be used to endorse or
promote products derived from this software without specific prior written
permission.

Disclaimer

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER AND CONTRIBUTORS ``AS IS'' AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, AND/OR ANY
WARRANTIES THAT THIS SOFTWARE IS FREE OF INFRINGEMENT ARE DISCLAIMED.

IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

// Maintainer: joaander

#include <boost/python.hpp>
#include <boost/python/suite/indexing/vector_indexing_suite.hpp>

using namespace boost::python;

#ifdef ENABLE_MPI
#include "Communicator.h"
#endif

#include <boost/bind.hpp>

#include "NeighborList.h"
#include "BondedGroupData.h"

#include <sstream>
#include <fstream>

#include <iostream>
#include <stdexcept>

using namespace boost;
using namespace std;

/*! \file NeighborList.cc
    \brief Defines the NeighborList class
*/

/*! \param sysdef System the neighborlist is to compute neighbors for
    \param r_cut Cuttoff radius under which particles are considered neighbors
    \param r_buff Buffere radius around \a r_cut in which neighbors will be included

    \post NeighborList is initialized and the list memory has been allocated,
        but the list will not be computed until compute is called.
    \post The storage mode defaults to half
*/
NeighborList::NeighborList(boost::shared_ptr<SystemDefinition> sysdef, Scalar _r_cut, Scalar r_buff)
    : Compute(sysdef), m_typpair_idx(m_pdata->getNTypes()), m_r_cut_max(_r_cut), m_r_buff(r_buff), m_d_max(1.0),
      m_filter_body(false), m_storage_mode(half), m_updates(0), m_forced_updates(0),
      m_dangerous_updates(0), m_force_update(true), m_dist_check(true), m_has_been_updated_once(false),
      m_want_exclusions(false)
    {
    m_exec_conf->msg->notice(5) << "Constructing Neighborlist" << endl;

    if (m_r_buff < 0.0)
        {
        m_exec_conf->msg->error() << "nlist: Requested buffer radius is less than zero" << endl;
        throw runtime_error("Error initializing NeighborList");
        }

    // there are no half storage modes now
//     setStorageMode(full);
    // need to force full storage mode in the individual class, since you could still use half storage in the binned
    // classes

    // initialize values
    m_last_updated_tstep = 0;
    m_last_checked_tstep = 0;
    m_last_check_result = false;
    m_every = 0;
//     m_Nmax = 0;
    m_exclusions_set = false;

    // initialize box length at last update
    m_last_L = m_pdata->getGlobalBox().getNearestPlaneDistance();
    m_last_L_local = m_pdata->getBox().getNearestPlaneDistance();

    // allocate conditions flags
//     GPUFlags<unsigned int> conditions(exec_conf);
//     m_conditions.swap(conditions);
    
    // allocate r_cut pair data for user inputs
    GPUArray<Scalar> r_cut(m_typpair_idx.getNumElements(), exec_conf);
    m_r_cut.swap(r_cut);
    
    // holds the maximum rcut on a per type basis
    GPUArray<Scalar> rcut_max(m_pdata->getNTypes(), m_exec_conf);
    m_rcut_max.swap(rcut_max);
    
    // allocate the full-blown r_listsq array, which we will pass to kernels
    GPUArray<Scalar> r_listsq(m_typpair_idx.getNumElements(), exec_conf);
    m_r_listsq.swap(r_listsq);
    
    // allocate m_n_neigh
    GPUArray<unsigned int> n_neigh(m_pdata->getMaxN(), exec_conf);
    m_n_neigh.swap(n_neigh);
    
    // allocate internal arrays to store and compute the head list
    GPUArray<unsigned int> head_list(m_pdata->getMaxN(), exec_conf);
    m_head_list.swap(head_list);
    GPUArray<unsigned int> Nmax(m_pdata->getNTypes(), exec_conf);
    m_Nmax.swap(Nmax);
    GPUArray<unsigned int> conditions(m_pdata->getNTypes(), exec_conf);
    m_conditions.swap(conditions);
    m_neigh_in_head = 0;

    // allocate neighbor list
    allocate();

    // allocate m_last_pos
    GPUArray<Scalar4> last_pos(m_pdata->getMaxN(), exec_conf);
    m_last_pos.swap(last_pos);

    // allocate initial memory allowing 4 exclusions per particle (will grow to match specified exclusions)
    GPUArray<unsigned int> n_ex_tag(m_pdata->getNGlobal(), exec_conf);
    m_n_ex_tag.swap(n_ex_tag);

    GPUArray<unsigned int> ex_list_tag(m_pdata->getNGlobal(), 1, exec_conf);
    m_ex_list_tag.swap(ex_list_tag);

    GPUArray<unsigned int> n_ex_idx(m_pdata->getMaxN(), exec_conf);
    m_n_ex_idx.swap(n_ex_idx);
    GPUArray<unsigned int> ex_list_idx(m_pdata->getMaxN(), 1, exec_conf);
    m_ex_list_idx.swap(ex_list_idx);

    // reset exclusions
    clearExclusions();

    m_ex_list_indexer = Index2D(m_ex_list_idx.getPitch(), 1);
    m_ex_list_indexer_tag = Index2D(m_ex_list_tag.getPitch(), 1);

    m_sort_connection = m_pdata->connectParticleSort(bind(&NeighborList::forceUpdate, this));

    m_max_particle_num_change_connection = m_pdata->connectMaxParticleNumberChange(bind(&NeighborList::reallocate, this));

    // allocate m_update_periods tracking info
    m_update_periods.resize(100);
    for (unsigned int i = 0; i < m_update_periods.size(); i++)
        m_update_periods[i] = 0;
    }

/*! Reallocate internal data structures upon change of local maximum particle number
 */
void NeighborList::reallocate()
    {
    m_last_pos.resize(m_pdata->getMaxN());
    m_n_ex_idx.resize(m_pdata->getMaxN());
    unsigned int ex_list_height = m_ex_list_indexer.getH();
    m_ex_list_idx.resize(m_pdata->getMaxN(), ex_list_height );
    m_ex_list_indexer = Index2D(m_ex_list_idx.getPitch(), ex_list_height);

//     m_nlist.resize(m_pdata->getMaxN(), m_Nmax+1);
//     m_nlist_indexer = Index2D(m_nlist.getPitch(), m_Nmax);

    m_n_neigh.resize(m_pdata->getMaxN());

    if (m_n_ex_tag.getNumElements() != m_pdata->getNGlobal())
        {
        m_n_ex_tag.resize(m_pdata->getNGlobal());
        m_ex_list_tag.resize(m_pdata->getNGlobal(), m_ex_list_tag.getHeight());
        m_ex_list_indexer_tag = Index2D(m_ex_list_tag.getPitch(), m_ex_list_tag.getHeight());

        // clear all exclusions
        clearExclusions();

        // they need to be added again
        m_want_exclusions = true;
        }
    }

NeighborList::~NeighborList()
    {
    m_exec_conf->msg->notice(5) << "Destroying Neighborlist" << endl;

    m_sort_connection.disconnect();
    m_max_particle_num_change_connection.disconnect();
#ifdef ENABLE_MPI
    if (m_migrate_request_connection.connected())
        m_migrate_request_connection.disconnect();
    if (m_comm_flags_request.connected())
        m_comm_flags_request.disconnect();
#endif
    }

/*! Updates the neighborlist if it has not yet been updated this times step
    \param timestep Current time step of the simulation
*/
void NeighborList::compute(unsigned int timestep)
    {
    // skip if we shouldn't compute this step
    if (!shouldCompute(timestep) && !m_force_update)
        return;

    if (m_prof) m_prof->push("Neighbor");
    
	// take care of some updates if things have changed since construction
    if (m_force_update)
        {
        updateRList();
        
        // build the head list since some sort of change (like a particle sort) happened
        buildHeadList();
        
		// update the exclusion data if this is a forced update        
        if (m_exclusions_set)
            updateExListIdx();
        }
            
    // check if the list needs to be updated and update it
    if (needsUpdating(timestep))
        {
        // rebuild the list until there is no overflow
        bool overflowed = false;
        do
            {
            buildNlist(timestep);

            overflowed = checkConditions();
            // if we overflowed, need to reallocate memory and reset the conditions
            if (overflowed)
                {
                allocate();
                buildHeadList();
                resetConditions();
                }
            } while (overflowed);

        if (m_exclusions_set)
            filterNlist();

        setLastUpdatedPos();
        m_has_been_updated_once = true;
        }
    if (m_prof) m_prof->pop();
    }

/*! \param num_iters Number of iterations to average for the benchmark
    \returns Milliseconds of execution time per calculation

    Calls buildNlist repeatedly to benchmark the neighbor list.
*/
double NeighborList::benchmark(unsigned int num_iters)
    {
    ClockSource t;
    // warm up run
    forceUpdate();
    compute(0);
    buildNlist(0);

#ifdef ENABLE_CUDA
    if (exec_conf->isCUDAEnabled())
        {
        cudaThreadSynchronize();
        CHECK_CUDA_ERROR();
        }
#endif

    // benchmark
    uint64_t start_time = t.getTime();
    for (unsigned int i = 0; i < num_iters; i++)
        buildNlist(0);

#ifdef ENABLE_CUDA
    if (exec_conf->isCUDAEnabled())
        cudaThreadSynchronize();
#endif
    uint64_t total_time_ns = t.getTime() - start_time;

    // convert the run time to milliseconds
    return double(total_time_ns) / 1e6 / double(num_iters);
    }

void NeighborList::setRCut(Scalar r_cut, Scalar r_buff)
    {
    }

/*! \param r_cut New cuttoff radius to set
    \note Changing the cuttoff radius does NOT immediately update the neighborlist.
            The new cuttoff will take effect when compute is called for the next timestep.
*/
void NeighborList::setRCutPair(unsigned int typ1, unsigned int typ2, Scalar r_cut)
    {
    // check that r_cut is not negative
    if (r_cut < 0.0)
        {
        m_exec_conf->msg->error() << "nlist: Requested cuttoff radius is less than zero" << endl;
        throw runtime_error("Error changing NeighborList parameters");
        }
        
    if (typ1 >= m_pdata->getNTypes() || typ2 >= m_pdata->getNTypes())
        {
        this->m_exec_conf->msg->error() << "nlist: Trying to set rcut for a non existant type! "
                  << typ1 << "," << typ2 << std::endl;
        throw std::runtime_error("Error changing NeighborList parameters");
        }
        
	// stash the potential rcuts, r_list will be computed on next forced update
    ArrayHandle<Scalar> h_r_cut(m_r_cut, access_location::host, access_mode::readwrite);
    h_r_cut.data[m_typpair_idx(typ1, typ2)] = r_cut;
    h_r_cut.data[m_typpair_idx(typ2, typ1)] = r_cut;

    // update the maximum cutoff of all those set so far
    // we loop in case there's a weird situation where you set once, and then reduce the cutoff
    // this should probably be moved to a separate function that only gets called once when an
    // update is needed, before each nlist build
    ArrayHandle<Scalar> h_rcut_max(m_rcut_max, access_location::host, access_mode::readwrite);    
    Scalar r_cut_max = 0.0f;
    for (unsigned int i=0; i < m_pdata->getNTypes(); ++i)
        {
        // get the maximum cutoff for this type
        Scalar r_cut_max_i = 0.0f;
        for (unsigned int j=0; j < m_pdata->getNTypes(); ++j)
            {
            if (h_r_cut.data[m_typpair_idx(i,j)] > r_cut_max_i)
                r_cut_max_i = h_r_cut.data[m_typpair_idx(i,j)];
            }
        h_rcut_max.data[i] = r_cut_max_i;
        if (r_cut_max_i > r_cut_max)
            r_cut_max = r_cut_max_i;
        }
    m_r_cut_max = r_cut_max;
        

        
#ifdef ENABLE_MPI
    if (m_comm)
        {
        Scalar r_list_max = m_r_cut_max + m_r_buff;
        m_comm->setGhostLayerWidth(r_list_max);
        m_comm->setRBuff(m_r_buff);
        }
#endif
    forceUpdate();
    }
    
/*! \param r_buff New buffer radius to set
    \note Changing the buffer radius does NOT immediately update the neighborlist.
            The new buffer will take effect when compute is called for the next timestep.
*/
void NeighborList::setRBuff(Scalar r_buff)
    {
    m_r_buff = r_buff;
    if (m_r_buff < 0.0)
        {
        m_exec_conf->msg->error() << "nlist: Requested buffer radius is less than zero" << endl;
        throw runtime_error("Error changing NeighborList parameters");
        }

#ifdef ENABLE_MPI
    if (m_comm)
        {
        Scalar r_list_max = m_r_cut_max + m_r_buff;
        m_comm->setGhostLayerWidth(r_list_max);
        m_comm->setRBuff(m_r_buff);
        }
#endif
    forceUpdate();
    } 
 
//! Loops through all pairs, and updates the r_list  
void NeighborList::updateRList()
	{
	// only need a read on the real cutoff
	ArrayHandle<Scalar> h_r_cut(m_r_cut, access_location::host, access_mode::read);
	
	// now we need to read and write on the r_list
	ArrayHandle<Scalar> h_r_listsq(m_r_listsq, access_location::host, access_mode::overwrite);
	
	for (unsigned int i=0; i < m_typpair_idx.getNumElements(); ++i)
		{
		Scalar r_list = h_r_cut.data[i] + m_r_buff;
		h_r_listsq.data[i] = r_list*r_list;
		}
	}

/*! \returns an estimate of the number of neighbors per particle
    This mean-field estimate may be very bad dending on how clustered particles are.
    Derived classes can override this method to provide better estimates.

    \note Under NO circumstances should calling this method produce any
    appreciable amount of overhead. This is mainly a warning to
    derived classes.
*/
Scalar NeighborList::estimateNNeigh()
    {
    // calculate a number density of particles
    BoxDim box = m_pdata->getBox();
    Scalar3 L = box.getL();
    Scalar vol = L.x * L.y * L.z;
    Scalar n_dens = Scalar(m_pdata->getN()) / vol;

    // calculate the average number of neighbors by multiplying by the volume
    // within the cutoff
    Scalar r_max = m_r_cut_max + m_r_buff;
    Scalar vol_cut = Scalar(4.0/3.0 * M_PI) * r_max * r_max * r_max;
    return n_dens * vol_cut;
    }

/*! \param tag1 TAG (not index) of the first particle in the pair
    \param tag2 TAG (not index) of the second particle in the pair
    \post The pair \a tag1, \a tag2 will not appear in the neighborlist
    \note This only takes effect on the next call to compute() that updates the list
    \note Duplicates are checked for and not added.
*/
void NeighborList::addExclusion(unsigned int tag1, unsigned int tag2)
    {
    assert(tag1 < m_pdata->getNGlobal());
    assert(tag2 < m_pdata->getNGlobal());

    m_exclusions_set = true;

    // don't add an exclusion twice
    if (isExcluded(tag1, tag2))
        return;

    // this is clunky, but needed due to the fact that we cannot have an array handle in scope when
    // calling grow exclusion list
    bool grow = false;
        {
        // access arrays
        ArrayHandle<unsigned int> h_n_ex_tag(m_n_ex_tag, access_location::host, access_mode::readwrite);

        // grow the list if necessary
        if (h_n_ex_tag.data[tag1] == m_ex_list_indexer.getH())
            grow = true;

        if (h_n_ex_tag.data[tag2] == m_ex_list_indexer.getH())
            grow = true;
        }

    if (grow)
        growExclusionList();

        {
        // access arrays
        ArrayHandle<unsigned int> h_ex_list_tag(m_ex_list_tag, access_location::host, access_mode::readwrite);
        ArrayHandle<unsigned int> h_n_ex_tag(m_n_ex_tag, access_location::host, access_mode::readwrite);

        // add tag2 to tag1's exclusion list
        unsigned int pos1 = h_n_ex_tag.data[tag1];
        assert(pos1 < m_ex_list_indexer.getH());
        h_ex_list_tag.data[m_ex_list_indexer_tag(tag1,pos1)] = tag2;
        h_n_ex_tag.data[tag1]++;

        // add tag1 to tag2's exclusion list
        unsigned int pos2 = h_n_ex_tag.data[tag2];
        assert(pos2 < m_ex_list_indexer.getH());
        h_ex_list_tag.data[m_ex_list_indexer_tag(tag2,pos2)] = tag1;
        h_n_ex_tag.data[tag2]++;
        }

    // Exclusions have been added, so assume the exclusion list is now current
    m_want_exclusions = false;

    forceUpdate();
    }

/*! \post No particles are excluded from the neighbor list
*/
void NeighborList::clearExclusions()
    {
    ArrayHandle<unsigned int> h_n_ex_tag(m_n_ex_tag, access_location::host, access_mode::overwrite);
    ArrayHandle<unsigned int> h_n_ex_idx(m_n_ex_idx, access_location::host, access_mode::overwrite);

    memset(h_n_ex_tag.data, 0, sizeof(unsigned int)*m_pdata->getNGlobal());
    memset(h_n_ex_idx.data, 0, sizeof(unsigned int)*m_pdata->getN());
    m_exclusions_set = false;

    forceUpdate();
    }

//! Get number of exclusions involving n particles
unsigned int NeighborList::getNumExclusions(unsigned int size)
    {
    ArrayHandle<unsigned int> h_n_ex_tag(m_n_ex_tag, access_location::host, access_mode::read);
    unsigned int count = 0;
    for (unsigned int i = 0; i < m_pdata->getN(); i++)
        {
        unsigned int num_excluded = h_n_ex_tag.data[i];

        if (num_excluded == size) count++;
        }

    #ifdef ENABLE_MPI
    if (m_pdata->getDomainDecomposition())
        {
        MPI_Allreduce(MPI_IN_PLACE,
                      &count,
                      1,
                      MPI_INT,
                      MPI_SUM,
                      m_exec_conf->getMPICommunicator());
        }
    #endif

    return count;
    }

/*! \post Gather some statistics about exclusions usage.
*/
void NeighborList::countExclusions()
    {
    unsigned int MAX_COUNT_EXCLUDED = 16;
    unsigned int excluded_count[MAX_COUNT_EXCLUDED+2];
    unsigned int num_excluded, max_num_excluded;

    ArrayHandle<unsigned int> h_n_ex_tag(m_n_ex_tag, access_location::host, access_mode::read);

    max_num_excluded = 0;
    for (unsigned int c=0; c <= MAX_COUNT_EXCLUDED+1; ++c)
        excluded_count[c] = 0;

    for (unsigned int i = 0; i < m_pdata->getNGlobal(); i++)
        {
        num_excluded = h_n_ex_tag.data[i];

        if (num_excluded > max_num_excluded)
            max_num_excluded = num_excluded;

        if (num_excluded > MAX_COUNT_EXCLUDED)
            num_excluded = MAX_COUNT_EXCLUDED + 1;

        excluded_count[num_excluded] += 1;
        }

    m_exec_conf->msg->notice(2) << "-- Neighborlist exclusion statistics -- :" << endl;
    for (unsigned int i=0; i <= MAX_COUNT_EXCLUDED; ++i)
        {
        if (excluded_count[i] > 0)
            m_exec_conf->msg->notice(2) << "Particles with " << i << " exclusions             : " << excluded_count[i] << endl;
        }

    if (excluded_count[MAX_COUNT_EXCLUDED+1])
        {
        m_exec_conf->msg->notice(2) << "Particles with more than " << MAX_COUNT_EXCLUDED << " exclusions: "
             << excluded_count[MAX_COUNT_EXCLUDED+1] << endl;
        }

    if (m_filter_body)
        m_exec_conf->msg->notice(2) << "Neighbors excluded when in the same body: yes" << endl;
    else
        m_exec_conf->msg->notice(2) << "Neighbors excluded when in the same body: no" << endl;

    if (!m_filter_body && m_sysdef->getRigidData()->getNumBodies() > 0)
        {
        m_exec_conf->msg->warning() << "Disabling the body exclusion will cause rigid bodies to behave erratically" << endl
             << "            unless inter-body pair forces are very small." << endl;
        }
    }

/*! After calling addExclusionFromBonds() all bonds specified in the attached ParticleData will be
    added as exlusions. Any additional bonds added after this will not be automatically added as exclusions.
*/
void NeighborList::addExclusionsFromBonds()
    {
    boost::shared_ptr<BondData> bond_data = m_sysdef->getBondData();

    // access bond data by snapshot
    BondData::Snapshot snapshot(bond_data->getNGlobal());
    bond_data->takeSnapshot(snapshot);

    // broadcast global bond list
    std::vector<BondData::members_t> bonds;

#ifdef ENABLE_MPI
    if (m_pdata->getDomainDecomposition())
        {
        if (m_exec_conf->getRank() == 0)
            bonds = snapshot.groups;

        bcast(bonds, 0, m_exec_conf->getMPICommunicator());
        }
    else
#endif
        {
        bonds = snapshot.groups;
        }

    // for each bond
    for (unsigned int i = 0; i < bonds.size(); i++)
        // add an exclusion
        addExclusion(bonds[i].tag[0], bonds[i].tag[1]);
    }

/*! After calling addExclusionsFromAngles(), all angles specified in the attached ParticleData will be added to the
    exclusion list. Only the two end particles in the angle are excluded from interacting.
*/
void NeighborList::addExclusionsFromAngles()
    {
    boost::shared_ptr<AngleData> angle_data = m_sysdef->getAngleData();

    // access angle data by snapshot
    AngleData::Snapshot snapshot(angle_data->getNGlobal());
    angle_data->takeSnapshot(snapshot);

    // broadcast global angle list
    std::vector<AngleData::members_t> angles;

#ifdef ENABLE_MPI
    if (m_pdata->getDomainDecomposition())
        {
        if (m_exec_conf->getRank() == 0)
            angles = snapshot.groups;

        bcast(angles, 0, m_exec_conf->getMPICommunicator());
        }
    else
#endif
        {
        angles = snapshot.groups;
        }

    // for each angle
    for (unsigned int i = 0; i < angles.size(); i++)
        addExclusion(angles[i].tag[0], angles[i].tag[2]);
    }

/*! After calling addExclusionsFromDihedrals(), all dihedrals specified in the attached ParticleData will be added to the
    exclusion list. Only the two end particles in the dihedral are excluded from interacting.
*/
void NeighborList::addExclusionsFromDihedrals()
    {
    boost::shared_ptr<DihedralData> dihedral_data = m_sysdef->getDihedralData();

    // access dihedral data by snapshot
    DihedralData::Snapshot snapshot(dihedral_data->getNGlobal());
    dihedral_data->takeSnapshot(snapshot);

    // broadcast global dihedral list
    std::vector<DihedralData::members_t> dihedrals;

#ifdef ENABLE_MPI
    if (m_pdata->getDomainDecomposition())
        {
        if (m_exec_conf->getRank() == 0)
            dihedrals = snapshot.groups;

        bcast(dihedrals, 0, m_exec_conf->getMPICommunicator());
        }
    else
#endif
        {
        dihedrals = snapshot.groups;
        }

    // for each dihedral
    for (unsigned int i = 0; i < dihedrals.size(); i++)
        addExclusion(dihedrals[i].tag[0], dihedrals[i].tag[3]);
    }

/*! \param tag1 First particle tag in the pair
    \param tag2 Second particle tag in the pair
    \return true if the particles \a tag1 and \a tag2 have been excluded from the neighbor list
*/
bool NeighborList::isExcluded(unsigned int tag1, unsigned int tag2)
    {
    assert(tag1 < m_pdata->getNGlobal());
    assert(tag2 < m_pdata->getNGlobal());

    ArrayHandle<unsigned int> h_n_ex_tag(m_n_ex_tag, access_location::host, access_mode::read);
    ArrayHandle<unsigned int> h_ex_list_tag(m_ex_list_tag, access_location::host, access_mode::read);

    unsigned int n_ex = h_n_ex_tag.data[tag1];
    for (unsigned int i = 0; i < n_ex; i++)
        {
        if (h_ex_list_tag.data[m_ex_list_indexer_tag(tag1,i)] == tag2)
            return true;
        }

    return false;
    }

/*! Add topologically derived exclusions for angles
 *
 * This excludes all non-bonded interactions between all pairs particles
 * that are bonded to the same atom.
 * To make the process quasi-linear scaling with system size we first
 * create a 1-d array the collects the number and index of bond partners.
 */
void NeighborList::addOneThreeExclusionsFromTopology()
    {
    boost::shared_ptr<BondData> bond_data = m_sysdef->getBondData();
    const unsigned int myNAtoms = m_pdata->getNGlobal();
    const unsigned int MAXNBONDS = 7+1; //! assumed maximum number of bonds per atom plus one entry for the number of bonds.
    const unsigned int nBonds = bond_data->getNGlobal();

    if (nBonds == 0)
        {
        m_exec_conf->msg->warning() << "nlist: No bonds defined while trying to add topology derived 1-3 exclusions" << endl;
        return;
        }

    // build a per atom list with all bonding partners from the list of bonds.
    unsigned int *localBondList = new unsigned int[MAXNBONDS*myNAtoms];
    memset((void *)localBondList,0,sizeof(unsigned int)*MAXNBONDS*myNAtoms);

    for (unsigned int i = 0; i < nBonds; i++)
        {
        // loop over all bonds and make a 1D exlcusion map
        Bond bondi = bond_data->getGroupByTag(i);
        const unsigned int tagA = bondi.a;
        const unsigned int tagB = bondi.b;

        // next, incremement the number of bonds, and update the tags
        const unsigned int nBondsA = ++localBondList[tagA*MAXNBONDS];
        const unsigned int nBondsB = ++localBondList[tagB*MAXNBONDS];

        if (nBondsA >= MAXNBONDS)
            {
            m_exec_conf->msg->error() << "nlist: Too many bonds to process exclusions for particle with tag: " << tagA << endl;
            m_exec_conf->msg->error() << "Maximum allowed is currently: " << MAXNBONDS-1 << endl;
            throw runtime_error("Error setting up toplogical exclusions in NeighborList");
            }

        if (nBondsB >= MAXNBONDS)
            {
            m_exec_conf->msg->error() << "nlist: Too many bonds to process exclusions for particle with tag: " << tagB << endl;
            m_exec_conf->msg->error() << "Maximum allowed is currently: " << MAXNBONDS-1 << endl;
            throw runtime_error("Error setting up toplogical exclusions in NeighborList");
            }

        localBondList[tagA*MAXNBONDS + nBondsA] = tagB;
        localBondList[tagB*MAXNBONDS + nBondsB] = tagA;
        }

    // now loop over the atoms and build exclusions if we have more than
    // one bonding partner, i.e. we are in the center of an angle.
    for (unsigned int i = 0; i < myNAtoms; i++)
        {
        // now, loop over all atoms, and find those in the middle of an angle
        const unsigned int iAtom = i*MAXNBONDS;
        const unsigned int nBonds = localBondList[iAtom];

        if (nBonds > 1) // need at least two bonds
            {
            for (unsigned int j = 1; j < nBonds; ++j)
                {
                for (unsigned int k = j+1; k <= nBonds; ++k)
                    addExclusion(localBondList[iAtom+j],localBondList[iAtom+k]);
                }
            }
        }
    // free temp memory
    delete[] localBondList;
    }

/*! Add topologically derived exclusions for dihedrals
 *
 * This excludes all non-bonded interactions between all pairs particles
 * that are connected to a common bond.
 *
 * To make the process quasi-linear scaling with system size we first
 * create a 1-d array the collects the number and index of bond partners.
 * and then loop over bonded partners.
 */
void NeighborList::addOneFourExclusionsFromTopology()
    {
    boost::shared_ptr<BondData> bond_data = m_sysdef->getBondData();
    const unsigned int myNAtoms = m_pdata->getNGlobal();
    const unsigned int MAXNBONDS = 7+1; //! assumed maximum number of bonds per atom plus one entry for the number of bonds.
    const unsigned int nBonds = bond_data->getNGlobal();

    if (nBonds == 0)
        {
        m_exec_conf->msg->warning() << "nlist: No bonds defined while trying to add topology derived 1-4 exclusions" << endl;
        return;
        }

    // allocate and clear data.
    unsigned int *localBondList = new unsigned int[MAXNBONDS*myNAtoms];
    memset((void *)localBondList,0,sizeof(unsigned int)*MAXNBONDS*myNAtoms);

    for (unsigned int i = 0; i < nBonds; i++)
        {
        // loop over all bonds and make a 1D exlcusion map
        Bond bondi = bond_data->getGroupByTag(i);
        const unsigned int tagA = bondi.a;
        const unsigned int tagB = bondi.b;

        // next, incrememt the number of bonds, and update the tags
        const unsigned int nBondsA = ++localBondList[tagA*MAXNBONDS];
        const unsigned int nBondsB = ++localBondList[tagB*MAXNBONDS];

        if (nBondsA >= MAXNBONDS)
            {
            m_exec_conf->msg->error() << "nlist: Too many bonds to process exclusions for particle with tag: " << tagA << endl;
            m_exec_conf->msg->error() << "Maximum allowed is currently: " << MAXNBONDS-1 << endl;
            throw runtime_error("Error setting up toplogical exclusions in NeighborList");
            }

        if (nBondsB >= MAXNBONDS)
            {
            m_exec_conf->msg->error() << "nlist: Too many bonds to process exclusions for particle with tag: " << tagB << endl;
            m_exec_conf->msg->error() << "Maximum allowed is currently: " << MAXNBONDS-1 << endl;
            throw runtime_error("Error setting up toplogical exclusions in NeighborList");
            }

        localBondList[tagA*MAXNBONDS + nBondsA] = tagB;
        localBondList[tagB*MAXNBONDS + nBondsB] = tagA;
        }

    //  loop over all bonds
    for (unsigned int i = 0; i < nBonds; i++)
        {
        Bond bondi = bond_data->getGroupByTag(i);
        const unsigned int tagA = bondi.a;
        const unsigned int tagB = bondi.b;

        const unsigned int nBondsA = localBondList[tagA*MAXNBONDS];
        const unsigned int nBondsB = localBondList[tagB*MAXNBONDS];

        for (unsigned int j = 1; j <= nBondsA; j++)
            {
            const unsigned int tagJ = localBondList[tagA*MAXNBONDS+j];
            if (tagJ == tagB) // skip the bond in the middle of the dihedral
                continue;

            for (unsigned int k = 1; k <= nBondsB; k++)
                {
                const unsigned int tagK = localBondList[tagB*MAXNBONDS+k];
                if (tagK == tagA) // skip the bond in the middle of the dihedral
                    continue;

                addExclusion(tagJ,tagK);
                }
            }
        }
    // free temp memory
    delete[] localBondList;
    }


/*! \returns true If any of the particles have been moved more than 1/2 of the buffer distance since the last call
        to this method that returned true.
    \returns false If none of the particles has been moved more than 1/2 of the buffer distance since the last call to this
        method that returned true.

    Note: this method relies on data set by setLastUpdatedPos(), which must be called to set the previous data used
    in the next call to distanceCheck();
*/
bool NeighborList::distanceCheck(unsigned int timestep)
    {
    ArrayHandle<Scalar4> h_pos(m_pdata->getPositions(), access_location::host, access_mode::read);

    // sanity check
    assert(h_pos.data);

    // profile
    if (m_prof) m_prof->push("Dist check");

    // temporary storage for the result
    bool result = false;

    // get a local copy of the simulation box too
    const BoxDim& box = m_pdata->getBox();

    // get current nearest plane distances
    Scalar3 L_g = m_pdata->getGlobalBox().getNearestPlaneDistance();

    // Find direction of maximum box length contraction (smallest eigenvalue of deformation tensor)
    Scalar3 lambda = L_g / m_last_L;
    Scalar lambda_min = (lambda.x < lambda.y) ? lambda.x : lambda.y;
    lambda_min = (lambda_min < lambda.z) ? lambda_min : lambda.z;
    
    ArrayHandle<Scalar4> h_last_pos(m_last_pos, access_location::host, access_mode::read);
    ArrayHandle<Scalar> h_rcut_max(m_rcut_max, access_location::host, access_mode::read);
    
    for (unsigned int i = 0; i < m_pdata->getN(); i++)
        {
        const unsigned int type_i = __scalar_as_int(h_pos.data[i].w);
        
        // max cutoff distance for inclusion in neighborlist if type i
        const Scalar rmax = h_rcut_max.data[type_i] + m_r_buff;
        // max displacement for each particle (after subtraction of homogeneous dilations)
        const Scalar delta_max = (rmax*lambda_min - h_rcut_max.data[type_i])/Scalar(2.0);
        Scalar maxsq = (delta_max > 0) ? delta_max*delta_max : 0;
        
        Scalar3 dx = make_scalar3(h_pos.data[i].x - lambda.x*h_last_pos.data[i].x,
                                  h_pos.data[i].y - lambda.y*h_last_pos.data[i].y,
                                  h_pos.data[i].z - lambda.z*h_last_pos.data[i].z);

        dx = box.minImage(dx);

        if (dot(dx, dx) >= maxsq)
            {
            result = true;
            break;
            }
        }

    #ifdef ENABLE_MPI
    if (m_pdata->getDomainDecomposition())
        {
        if (m_prof) m_prof->push("MPI allreduce");
        // check if migrate criterium is fulfilled on any rank
        int local_result = result ? 1 : 0;
        int global_result = 0;
        MPI_Allreduce(&local_result,
            &global_result,
            1,
            MPI_INT,
            MPI_MAX,
            m_exec_conf->getMPICommunicator());
        result = (global_result > 0);
        if (m_prof) m_prof->pop();
        }
    #endif

    // don't worry about computing flops here, this is fast
    if (m_prof) m_prof->pop();

    return result;
    }

/*! Copies the current positions of all particles over to m_last_x etc...
*/
void NeighborList::setLastUpdatedPos()
    {
    ArrayHandle<Scalar4> h_pos(m_pdata->getPositions(), access_location::host, access_mode::read);

    // sanity check
    assert(h_pos.data);

    // profile
    if (m_prof) m_prof->push("Dist check");

    // update the last position arrays
    ArrayHandle<Scalar4> h_last_pos(m_last_pos, access_location::host, access_mode::overwrite);
    for (unsigned int i = 0; i < m_pdata->getN(); i++)
        {
        h_last_pos.data[i] = make_scalar4(h_pos.data[i].x, h_pos.data[i].y, h_pos.data[i].z, Scalar(0.0));
        }

    // update last box nearest plane distance
    m_last_L = m_pdata->getGlobalBox().getNearestPlaneDistance();
    m_last_L_local = m_pdata->getBox().getNearestPlaneDistance();

    if (m_prof) m_prof->pop();
    }

bool NeighborList::shouldCheckDistance(unsigned int timestep)
    {
    return !m_force_update && !(timestep < (m_last_updated_tstep + m_every));
    }

/*! \returns true If the neighbor list needs to be updated
    \returns false If the neighbor list does not need to be updated
    \note This is designed to be called if (needsUpdating()) then update every step.
        It internally handles many state variables that rely on this assumption.

    \param timestep Current time step in the simulation
*/
bool NeighborList::needsUpdating(unsigned int timestep)
    {
    if (m_last_checked_tstep == timestep)
        {
        if (m_force_update)
            {
            // force update is counted only once per time step
            m_force_update = false;
            return true;
            }
        return m_last_check_result;
        }

    m_last_checked_tstep = timestep;

    if (!m_force_update && !shouldCheckDistance(timestep))
        {
        m_last_check_result = false;
        return false;
        }

    // temporary storage for return result
    bool result = false;

    // check if this is a dangerous time
    // we are dangerous if m_every is greater than 1 and this is the first check after the
    // last build
    bool dangerous = false;
    if (m_dist_check && (m_every > 1 && timestep == (m_last_updated_tstep + m_every)))
        dangerous = true;

    // if the update has been forced, the result defaults to true
    if (m_force_update)
        {
        result = true;
        m_force_update = false;
        m_forced_updates += 1;
        m_last_updated_tstep = timestep;

        // when an update is forced, there is no way to tell if the build
        // is dangerous or not: filter out the false positive errors
        dangerous = false;
        }
    else
        {
        // not a forced update, perform the distance check to determine
        // if the list needs to be updated - no dist check needed if r_buff is tiny
        // it also needs to be updated if m_every is 0, or the check period is hit when distance checks are disabled
        if (m_r_buff < 1e-6 ||
            (!m_dist_check && (m_every == 0 || (m_every > 1 && timestep == (m_last_updated_tstep + m_every)))))
            {
            result = true;
            }
        else
            {
            result = distanceCheck(timestep);
            }

        if (result)
            {
            // record update histogram - but only if the period is positive
            if (timestep > m_last_updated_tstep)
                {
                unsigned int period = timestep - m_last_updated_tstep;
                if (period >= m_update_periods.size())
                    period = m_update_periods.size()-1;
                m_update_periods[period]++;
                }

            m_last_updated_tstep = timestep;
            m_updates += 1;
            }
        }

    // warn the user if this is a dangerous build
    if (result && dangerous)
        {
        m_exec_conf->msg->notice(2) << "nlist: Dangerous neighborlist build occured. Continuing this simulation may produce incorrect results and/or program crashes. Decrease the neighborlist check_period and rerun." << endl;
        m_dangerous_updates += 1;
        }

    m_last_check_result = result;
    return result;
    }

/*! Generic statistics that apply to any neighbor list, like the number of updates,
    average number of neighbors, etc... are printed to stdout. Derived classes should
    print any pertinient information they see fit to.
 */
void NeighborList::printStats()
    {
    // return earsly if the notice level is less than 1
    if (m_exec_conf->msg->getNoticeLevel() < 1)
        return;

    m_exec_conf->msg->notice(1) << "-- Neighborlist stats:" << endl;
    m_exec_conf->msg->notice(1) << m_updates << " normal updates / " << m_forced_updates << " forced updates / " << m_dangerous_updates << " dangerous updates" << endl;

    // access the number of neighbors to generate stats
    ArrayHandle<unsigned int> h_n_neigh(m_n_neigh, access_location::host, access_mode::read);

    // build some simple statistics of the number of neighbors
    unsigned int n_neigh_min = m_pdata->getN();
    unsigned int n_neigh_max = 0;
    Scalar n_neigh_avg = 0.0;

    for (unsigned int i = 0; i < m_pdata->getN(); i++)
        {
        unsigned int n_neigh = (unsigned int)h_n_neigh.data[i];
        if (n_neigh < n_neigh_min)
            n_neigh_min = n_neigh;
        if (n_neigh > n_neigh_max)
            n_neigh_max = n_neigh;

        n_neigh_avg += Scalar(n_neigh);
        }

    // divide to get the average
    n_neigh_avg /= Scalar(m_pdata->getN());
    m_exec_conf->msg->notice(1) << "n_neigh_min: " << n_neigh_min << " / n_neigh_max: " << n_neigh_max << " / n_neigh_avg: " << n_neigh_avg << endl;

    m_exec_conf->msg->notice(1) << "shortest rebuild period: " << getSmallestRebuild() << endl;
    }

void NeighborList::resetStats()
    {
    m_updates = m_forced_updates = m_dangerous_updates = 0;

    for (unsigned int i = 0; i < m_update_periods.size(); i++)
        m_update_periods[i] = 0;
    }

unsigned int NeighborList::getSmallestRebuild()
    {
    for (unsigned int i = 0; i < m_update_periods.size(); i++)
        {
        if (m_update_periods[i] != 0)
            return i;
        }
    return m_update_periods.size();
    }

/*! Loops through the particles and finds all of the particles \c j who's distance is less than
    \param timestep Current time step of the simulation
    \c r_cut \c + \c r_buff from particle \c i, includes either i < j or all neighbors depending
    on the mode set by setStorageMode()
*/
void NeighborList::buildNlist(unsigned int timestep)
    {
    m_exec_conf->msg->error() << "nlist: O(N^2) neighbor lists are no longer supported." << endl;
    throw runtime_error("Error updating neighborlist bins");
    }

/*! Translates the exclusions set in \c m_n_ex_tag and \c m_ex_list_tag to indices in \c m_n_ex_idx and \c m_ex_list_idx
*/
void NeighborList::updateExListIdx()
    {
    if (m_prof)
        m_prof->push("update-ex");
    // access data
    ArrayHandle<unsigned int> h_tag(m_pdata->getTags(), access_location::host, access_mode::read);
    ArrayHandle<unsigned int> h_rtag(m_pdata->getRTags(), access_location::host, access_mode::read);

    ArrayHandle<unsigned int> h_n_ex_tag(m_n_ex_tag, access_location::host, access_mode::read);
    ArrayHandle<unsigned int> h_ex_list_tag(m_ex_list_tag, access_location::host, access_mode::read);
    ArrayHandle<unsigned int> h_n_ex_idx(m_n_ex_idx, access_location::host, access_mode::overwrite);
    ArrayHandle<unsigned int> h_ex_list_idx(m_ex_list_idx, access_location::host, access_mode::overwrite);

    // translate the number and exclusions from one array to the other
    for (unsigned int idx = 0; idx < m_pdata->getN(); idx++)
        {
        // get the tag for this index
        unsigned int tag = h_tag.data[idx];

        // copy the number of exclusions over
        unsigned int n = h_n_ex_tag.data[tag];
        h_n_ex_idx.data[idx] = n;

        // construct the exclusion list
        for (unsigned int offset = 0; offset < n; offset++)
            {
            unsigned int ex_tag = h_ex_list_tag.data[m_ex_list_indexer_tag(tag,offset)];
            unsigned int ex_idx = h_rtag.data[ex_tag];

            // store excluded particle idx
            h_ex_list_idx.data[m_ex_list_indexer(idx, offset)] = ex_idx;
            }
        }

    if (m_prof)
        m_prof->pop();
    }

/*! Loops through the neighbor list and filters out any excluded pairs
*/
void NeighborList::filterNlist()
    {
    if (m_prof)
        m_prof->push("filter");

    // access data
    ArrayHandle<unsigned int> h_head_list(m_head_list, access_location::host, access_mode::read);
    ArrayHandle<unsigned int> h_n_ex_idx(m_n_ex_idx, access_location::host, access_mode::read);
    ArrayHandle<unsigned int> h_ex_list_idx(m_ex_list_idx, access_location::host, access_mode::read);
    ArrayHandle<unsigned int> h_n_neigh(m_n_neigh, access_location::host, access_mode::readwrite);
    ArrayHandle<unsigned int> h_nlist(m_nlist, access_location::host, access_mode::readwrite);

    // for each particle's neighbor list
    for (unsigned int idx = 0; idx < m_pdata->getN(); idx++)
        {
        unsigned int myHead = h_head_list.data[idx];
        unsigned int n_neigh = h_n_neigh.data[idx];
        unsigned int n_ex = h_n_ex_idx.data[idx];
        unsigned int new_n_neigh = 0;

        // loop over the list, regenerating it as we go
        for (unsigned int cur_neigh_idx = 0; cur_neigh_idx < n_neigh; cur_neigh_idx++)
            {
            unsigned int cur_neigh = h_nlist.data[myHead + cur_neigh_idx];

            // test if excluded
            bool excluded = false;
            for (unsigned int cur_ex_idx = 0; cur_ex_idx < n_ex; cur_ex_idx++)
                {
                unsigned int cur_ex = h_ex_list_idx.data[m_ex_list_indexer(idx, cur_ex_idx)];
                if (cur_ex == cur_neigh)
                    {
                    excluded = true;
                    break;
                    }
                }

            // add it back to the list if it is not excluded
            if (!excluded)
                {
                h_nlist.data[myHead + new_n_neigh] = cur_neigh;
                new_n_neigh++;
                }
            }

        // update the number of neighbors
        h_n_neigh.data[idx] = new_n_neigh;
        }

    if (m_prof)
        m_prof->pop();
    }

void NeighborList::allocate()
    {
    // round up to the nearest multiple of 8
    ArrayHandle<unsigned int> h_Nmax(m_Nmax, access_location::host, access_mode::readwrite);
    for (unsigned int i=0; i < m_pdata->getNTypes(); ++i)
        {
        h_Nmax.data[i] = (h_Nmax.data[i] > 8) ? (h_Nmax.data[i] + 7) & ~7 : 8;
        
//      switching to multiples of 32 gives a 2% performance boost in LJ benchmarking, but why? omit for now
//         h_Nmax.data[i] = (h_Nmax.data[i] > 32) ? (h_Nmax.data[i] + 31) & ~31 : 32;
        }

    m_exec_conf->msg->notice(6) << "nlist: (Re-)Allocating neighbor list" << endl;
    
    // need to be careful about all of this reallocation if memory is already large... may need changing
    
    // allocate memory for the head list
    GPUArray<unsigned int> head_list(m_pdata->getMaxN(), exec_conf);
    m_head_list.swap(head_list);
    }

//! Slow serial build for the head list
void NeighborList::buildHeadList()
    {   
    if (m_prof) m_prof->push("head-list");
    
    ArrayHandle<unsigned int> h_head_list(m_head_list, access_location::host, access_mode::overwrite);
    ArrayHandle<Scalar4> h_pos(m_pdata->getPositions(), access_location::host, access_mode::read);
    ArrayHandle<unsigned int> h_Nmax(m_Nmax, access_location::host, access_mode::read);
    
    unsigned int headAddress = 0;
    for (unsigned int i=0; i < m_pdata->getN(); ++i)
        {
        h_head_list.data[i] = headAddress;

        // move the head address along
        unsigned int myType = __scalar_as_int(h_pos.data[i].w);
        headAddress += h_Nmax.data[myType];
        }

    // re-allocate the neighbor list
    // needs logical check
    if (headAddress > m_nlist.getPitch())
        {
        GPUArray<unsigned int> nlist(headAddress, exec_conf);
        m_nlist.swap(nlist);
        }
    if (m_prof) m_prof->pop();
    }

bool NeighborList::checkConditions()
    {
    bool result = false;

    ArrayHandle<unsigned int> h_conditions(m_conditions, access_location::host, access_mode::read);
    ArrayHandle<unsigned int> h_Nmax(m_Nmax, access_location::host, access_mode::readwrite);
    for (unsigned int i=0; i < m_pdata->getNTypes(); ++i)
        {
        if (h_conditions.data[i] > h_Nmax.data[i])
            {
            h_Nmax.data[i] = h_conditions.data[i];
            result = true;
            }
        }

    return result;
    }

void NeighborList::resetConditions()
    {
    ArrayHandle<unsigned int> h_conditions(m_conditions, access_location::host, access_mode::overwrite);
    for (unsigned int i=0; i < m_pdata->getNTypes(); ++i)
        {
        h_conditions.data[i] = 0;
        }
    }

void NeighborList::growExclusionList()
    {
    unsigned int new_height = m_ex_list_indexer.getH() + 1;

    m_ex_list_tag.resize(m_pdata->getNGlobal(), new_height);
    m_ex_list_idx.resize(m_pdata->getMaxN(), new_height);

    // update the indexers
    m_ex_list_indexer = Index2D(m_ex_list_idx.getPitch(), new_height);
    m_ex_list_indexer_tag = Index2D(m_ex_list_tag.getPitch(), new_height);

    // we didn't copy data for the new idx list, force an update so it will be correct
    forceUpdate();
    }

#ifdef ENABLE_MPI
//! Set the communicator to use
void NeighborList::setCommunicator(boost::shared_ptr<Communicator> comm)
    {
    if (!m_comm)
        {
        // only add the migrate request on the first call
        m_migrate_request_connection = comm->addMigrateRequest(bind(&NeighborList::peekUpdate, this, _1));
        m_comm_flags_request = comm->addCommFlagsRequest(bind(&NeighborList::getRequestedCommFlags, this, _1));
        }

    if (comm)
        {
        m_comm = comm;

        Scalar r_list_max = m_r_cut_max + m_r_buff;
        m_comm->setGhostLayerWidth(r_list_max);
        m_comm->setRBuff(m_r_buff);
        }
    }

//! Returns true if the particle migration criterium is fulfilled
/*! \note The criterium for when to request particle migration is the same as the one for neighbor list
    rebuilds, which is implemented in needsUpdating().
 */
bool NeighborList::peekUpdate(unsigned int timestep)
    {
    if (m_prof) m_prof->push("Neighbor");

    bool result = needsUpdating(timestep);

    if (m_prof) m_prof->pop();

    return result;
    }
#endif

void export_NeighborList()
    {
    class_< std::vector<unsigned int> >("std_vector_uint")
    .def(vector_indexing_suite<std::vector<unsigned int> >())
    .def("push_back", &std::vector<unsigned int>::push_back)
    ;

    scope in_nlist = class_<NeighborList, boost::shared_ptr<NeighborList>, bases<Compute>, boost::noncopyable >
                     ("NeighborList", init< boost::shared_ptr<SystemDefinition>, Scalar, Scalar >())
                     .def("setRCut", &NeighborList::setRCut)
                     .def("setRCutPair", &NeighborList::setRCutPair)
                     .def("setRBuff", &NeighborList::setRBuff)
                     .def("setEvery", &NeighborList::setEvery)
                     .def("setStorageMode", &NeighborList::setStorageMode)
                     .def("addExclusion", &NeighborList::addExclusion)
                     .def("clearExclusions", &NeighborList::clearExclusions)
                     .def("countExclusions", &NeighborList::countExclusions)
                     .def("addExclusionsFromBonds", &NeighborList::addExclusionsFromBonds)
                     .def("addExclusionsFromAngles", &NeighborList::addExclusionsFromAngles)
                     .def("addExclusionsFromDihedrals", &NeighborList::addExclusionsFromDihedrals)
                     .def("addOneThreeExclusionsFromTopology", &NeighborList::addOneThreeExclusionsFromTopology)
                     .def("addOneFourExclusionsFromTopology", &NeighborList::addOneFourExclusionsFromTopology)
                     .def("setFilterBody", &NeighborList::setFilterBody)
                     .def("getFilterBody", &NeighborList::getFilterBody)
                     .def("setMaximumDiameter", &NeighborList::setMaximumDiameter)
                     .def("getMaximumDiameter", &NeighborList::getMaximumDiameter)
                     .def("forceUpdate", &NeighborList::forceUpdate)
                     .def("estimateNNeigh", &NeighborList::estimateNNeigh)
                     .def("getSmallestRebuild", &NeighborList::getSmallestRebuild)
                     .def("getNumUpdates", &NeighborList::getNumUpdates)
                     .def("getNumExclusions", &NeighborList::getNumExclusions)
                     .def("wantExclusions", &NeighborList::wantExclusions)
#ifdef ENABLE_MPI
                     .def("setCommunicator", &NeighborList::setCommunicator)
#endif
                     ;

    enum_<NeighborList::storageMode>("storageMode")
    .value("half", NeighborList::half)
    .value("full", NeighborList::full)
    ;
    }
