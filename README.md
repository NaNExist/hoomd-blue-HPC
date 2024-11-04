
This is a copy of **HOOMD-blue** to optimize for **2024 APAC HPC-AI Competition**.

The difference from original **HOOMD-blue** is that we have added following codes to `CMakeLists.txt`:

```cmake
add_compile_options("-xhost -DMKL_ILP64 -qmkl-ilp64=parallel -fiopenmp -Wnan-infinity-disabled -Woverriding- option")
```

We also optimize the compile parameters, add the support of IntelMKL and use IntelMPI to replace OpenMPI. We also turn `ENABLE_TBB` to `on` to enable TBB support.

The main platform is **Gadi**.

## Resources

- [Compilation guide](BUILDING.md):
  Instructions for compiling and running **HOOMD-blue** on **Gadi**.

