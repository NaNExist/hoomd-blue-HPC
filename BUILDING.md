Building from source
====================

The following steps are designed to build HOOMD-blue from source on **Gadi**.

To build the **HOOMD-blue** from source:


## Obtain the source:

   ``` bash
mkdir ${HOME}/scratch/workdir/hoomd -p
time git -C ${HOME}/scratch/workdir/hoomd clone https://github.com/NaNExist/hoomd-blue-HPC.git  --recursive
time git -C ${HOME}/scratch/workdir/hoomd clone https://github.com/glotzerlab/hoomd-benchmarks
   ```
       
## Create Python 3 environment

The following commands

1. Set up a Python 3 Conda environment
2. Install latest `pybind11(2.13.5)` with PIP, to fix `pybind11 version(v2.10.1)` issue caused by wrong HOOMD-blue prerequisite list in the HOOMD-blue git code
3. Run`install-prereq-headers.py` scripts that provided by HOOMD-blue developers
4. Install `GSD` and `numpy`

```bash
time conda create -p ${HOME}/scratch/workdir/hoomd/hoomd.py312 python=3.12 -y
time ${HOME}/scratch/workdir/hoomd/hoomd.py312/bin/pip install pybind11
time ${HOME}/scratch/workdir/hoomd/hoomd.py312/bin/python3 ${HOME}/scratch/workdir/hoomd/hoomd-blue/install-prereq-headers.py -y
time ${HOME}/scratch/workdir/hoomd/hoomd.py312/bin/pip install numpy gsd
```

## Build HOOMD-blue with OpenMPI

The following commands

1. Check available MPI libraries, IntelMKL libraries and IntelTBB libraries that pre-built by the Supercomputer administrator
2. Load IntelMPI environment variables
3. Configure the scripts for building HOOMD-blue with IntelMPI and pip-installed `pybind11`
4. Build the Python package
5. Validate the built Python package
   
```bash
module purge
module load intel-compiler-llvm/2024.2.1
module load intel-mkl/2024.2.1
module load intel-mpi/2021.13.1
module load intel-tbb/2021.13.1

export TBB_DIR=/apps/intel-tools/intel-tbb/2021.13.1
export MKLROOT=/apps/intel-tools/intel-mkl/2024.2.1

time PATH=${HOME}/scratch/workdir/hoomd/hoomd.py312/bin:$PATH \
cmake \
-B ${HOME}/scratch/workdir/hoomd/build/hoomd-intelmpi \
-S ${HOME}/scratch/workdir/hoomd/hoomd-blue-HPC \
-D ENABLE_MPI=on \
-D ENABLE_TBB=on \
-D ENABLE_LLVM=off \
-DCMAKE_CXX_FLAGS=-march=native -DCMAKE_C_FLAGS=-march=native \
-D cereal_DIR=${HOME}/scratch/workdir/hoomd/hoomd.py312/lib64/cmake/cereal \
-D Eigen3_DIR=${HOME}/scratch/workdir/hoomd/hoomd.py312/share/eigen3/cmake \
-D pybind11_DIR=${HOME}/scratch/workdir/hoomd/hoomd.py312/lib/python3.12/site-packages/pybind11/share/cmake/pybind11

time cmake --build ${HOME}/scratch/workdir/hoomd/build/hoomd-intelmpi -j 8

PYTHONPATH=${HOME}/scratch/workdir/hoomd/build/hoomd-intelmpi \
${HOME}/scratch/workdir/hoomd/hoomd.py312/bin/python \
-m hoomd
# python: No module named hoomd.__main__; 'hoomd' is a package and cannot be directly executed
```

## Resolve the LOG_CAT_ML HCOLL issue

To address the HCOLL issue in the pre-built OpenMPI, where you encounter the following error

`[LOG_CAT_ML] component basesmuma is not available but requested in hierarchy: basesmuma,basesmuma,ucx_p2p:basesmsocket,basesmuma,p2p
[LOG_CAT_ML] ml_discover_hierarchy exited with error`

Follow these steps:

1. Download and unpack HPC-X
2. Load HPC-X OpenMPI module file instead of prebuilt openmpi/4.1.5 before launching the MPI task

```bash
time wget -P ${HOME} https://content.mellanox.com/hpc/hpc-x/v2.20/hpcx-v2.20-gcc-mlnx_ofed-redhat8-cuda12-x86_64.tbz
time tar -C ${HOME} -xf ${HOME}/hpcx-v2.20-gcc-mlnx_ofed-redhat8-cuda12-x86_64.tbz
```

## Create PBS bash script

Create a shell script file, `${HOME}/run/hoomd.sh`, with following contents

```bash
#!/bin/bash
#PBS -j oe
#PBS -M youremail@yourdomain.com
#PBS -m abe
#PBS -P yourprojectid
#PBS -l ngpus=0
#PBS -l walltime=00:10:00
##PBS -l other=hyperthread
#-report-bindings \

module purge
module load ${HOME}/hpcx-v2.20-gcc-mlnx_ofed-redhat8-cuda12-x86_64/modulefiles/hpcx-ompi
module load intel-mpi/2021.13.1

hosts=$(sort -u ${PBS_NODEFILE} | paste -sd ',')

cmd="time mpirun \
-hosts ${hosts} \
-wdir ${HOME}/scratch/workdir/hoomd \
-ppn 48 \
-genv PYTHONPATH ${HOME}/scratch/workdir/hoomd/build/hoomd-intelmpi:${HOME}/scratch/workdir/hoomd/hoomd-benchmarks \
${HOME}/scratch/workdir/hoomd/hoomd.py312/bin/python \
-m hoomd_benchmarks.md_pair_wca \
--device CPU -v \
-N 200000 --repeat ${REPEATS} \
--warmup_steps ${WARMUP_STEPS} --benchmark_steps ${BENCHMARK_STEPS}"

echo ${cmd}

exec ${cmd}
```
## Submit the job script to PBS

Create a shell script file, `${HOME}/run/submit.sh`, with following contents


The following script will help you to submit easily the job to the PBS queue.

```bash
#!/bin/bash  
  
if [ "$#" -ne 2 ]; then  
    echo "Usage: $0 nodes repeats"  
    echo "  nodes:        Number of nodes to request"
    echo "  repeats:      Number of script to repeat"  
    exit 1  
fi  
  
nodes=$1  
walltime=00:00:600  
repeats=$2 

if (($nodes<16))
then
        warmup_steps=40000
else
        warmup_steps=10000
fi

if (($nodes<16))
then
        benchmark_steps=80000
else
        benchmark_steps=`expr $nodes \* 10000`
fi

ncpus=$((48 * nodes))  
mem=$((nodes * 48 * 1))
  
job_name="hoomd.nodes${nodes}.WS${warmup_steps}.BS${benchmark_steps}"  
  
export NODES=$nodes  
export WARMUP_STEPS=$warmup_steps  
export BENCHMARK_STEPS=$benchmark_steps  
export REPEATS=$repeats

commands="qsub -V   
    -l walltime=${walltime},ncpus=$ncpus,mem=${mem}gb 
    -N ${job_name}   
    ./hoomd.sh"

echo "repeats = $repeats"
echo $commands

exec $commands
```

Then use the following command to submit the job

```bash
bash ${HOME}/run/submit.sh 16 1
```
