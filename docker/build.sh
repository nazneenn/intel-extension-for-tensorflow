#!/usr/bin/env bash
#
# Copyright (c) 2021-2022 Intel Corporation
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#    http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

set -e
IMAGE_TYPE=$1
if [ $IMAGE_TYPE == "gpu" ]
then
        IMAGE_NAME=intel-extension-for-tensorflow:gpu
        docker build --build-arg UBUNTU_VERSION=22.04 \
                                --build-arg ICD_VER=23.05.25593.18-601~22.04 \
                                --build-arg LEVEL_ZERO_GPU_VER=1.3.25593.18-601~22.04 \
                                --build-arg LEVEL_ZERO_VER=1.9.4+i589~22.04 \
                                --build-arg TF_VER=2.12 \
                                --build-arg DPCPP_VER=2023.1.0-46305 \
                                --build-arg MKL_VER=2023.1.0-46342 \
                                --build-arg PYTHON=python3.10 \
                                --build-arg TF_PLUGIN_WHEEL=intel_extension_for_tensorflow*.whl \
                                -t $IMAGE_NAME \
				-f itex-gpu.Dockerfile .
elif [ $IMAGE_TYPE == "gpu-horovod" ]
then
        IMAGE_NAME=intel-extension-for-tensorflow:gpu-horovod
        docker build --build-arg UBUNTU_VERSION=22.04 \
                                --build-arg ICD_VER=23.05.25593.18-601~22.04 \
                                --build-arg LEVEL_ZERO_GPU_VER=1.3.25593.18-601~22.04 \
                                --build-arg LEVEL_ZERO_VER=1.9.4+i589~22.04 \
                                --build-arg LEVEL_ZERO_DEV_VER=1.9.4+i589~22.04 \
                                --build-arg TF_VER=2.12 \
                                --build-arg DPCPP_VER=2023.1.0-46305 \
                                --build-arg MKL_VER=2023.1.0-46342 \
                                --build-arg CCL_VER=2021.9.0-43543 \
                                --build-arg PYTHON=python3.10 \
                                --build-arg HOROVOD_WHL=intel_optimization_for_horovod-*.whl \
                                --build-arg TF_PLUGIN_WHEEL=intel_extension_for_tensorflow*.whl \
                                -t $IMAGE_NAME \
                                -f itex-gpu-horovod.Dockerfile .
elif  [ $IMAGE_TYPE == "cpu-centos" ]
then
        IMAGE_NAME=intel-extension-for-tensorflow:cpu-centos
        docker build --build-arg CENTOS_VER=8 \
                                --build-arg PY_VER=39 \
                                --build-arg PYTHON=python3.9 \
                                --build-arg TF_VER=2.12 \
                                --build-arg TF_PLUGIN_WHEEL=intel_extension_for_tensorflow*.whl \
                                -t $IMAGE_NAME \
                                -f itex-cpu-centos.Dockerfile .
else
        IMAGE_NAME=intel-extension-for-tensorflow:cpu-ubuntu
        docker build --build-arg UBUNTU_VERSION=20.04 \
                                --build-arg PYTHON=python3.9 \
                                --build-arg TF_VER=2.12 \
                                --build-arg TF_PLUGIN_WHEEL=intel_extension_for_tensorflow*.whl \
                                -t $IMAGE_NAME \
                                -f itex-cpu-ubuntu.Dockerfile .
fi

