# Conda Environment Installation Instructions

This document provides a recommended installation instruction for Intel® Extension for TensorFlow* v1.2.0 and Intel® Optimization for Horovod* v0.5.0 deployment for distributed training on Conda environment.    

## Preconditions
Assume user has installed the Intel GPU driver and the required components of oneAPI Base Toolkit Packages as per [instructions](../install_for_gpu.md#install_oneapi_base_toolkit_packages) successfully. 


## Step by step instructions:

Miniconda is the recommended approach for installing stock TensorFlow. It creates a separate environment to avoid changing any installed software in your system. This is also the easiest way to install the required software especially for the GPU setup.
You can use the following command to install Miniconda. During installation, you may need to press enter and type "yes". Skip this step, if you have already insall conda.

```
curl https://repo.anaconda.com/miniconda/Miniconda3-latest-Linux-x86_64.sh -o Miniconda3-latest-Linux-x86_64.sh
bash Miniconda3-latest-Linux-x86_64.sh
```

You may need to restart your terminal or source ~/.bashrc to enable the conda command. Use conda -V to test if it is installed successfully.
```
conda -V
```
You may need to update your conda first, as at least conda 4.1.11 is required.
```
conda update conda

#Take Intel Python 2023.1.0 as an example to conda environment, but generic Python is also recommended.
conda create -n itex -c intel intelpython3_full==2023.1.0 python=3.9
```

Activate the environment by the following commands.
```
conda activate itex
```
Install stock Tensorflow 2.12.0 and Intel® Extension for TensorFlow* GPU wheels.
```
pip install --upgrade pip
pip install tensorflow==2.12.0
pip install intel-extension-for-tensorflow[gpu]
```

Set required environment variables:

```
# DPC++ compiler/oneMKL
source /path/to/intel/oneapi/compiler/latest/env/vars.sh
source /path/to/intel/oneapi/mkl/latest/env/vars.sh
```

Check the environment for GPU:
```bash
bash /path to site-packages/intel_extension_for_tensorflow/tools/env_check.sh
```
Verify install:
```
python3 -c "import intel_extension_for_tensorflow as itex; print(itex.version.GIT_VERSION)"
```
Expected result:
```
v1.2.0-8e0294ba
```

In order to install Intel® Optimization for Horovod* v0.5.0 for distributed training, oneCCL need to be installed at first when you install oneAPI Basekit.

```
source /path/to/intel/oneapi/mpi/latest/env/vars.sh
source /path/to/intel/oneapi/ccl/latest/env/vars.sh

pip install intel-optimization-for-horovod
```

Test multi-node with Intel MPI on cluster System, please set environment variables.
```
export FI_PROVIDER=sockets
```
