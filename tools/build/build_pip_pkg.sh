#!/usr/bin/env bash
# Copyright 2021 The TensorFlow NUFFT Authors. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# ==============================================================================
set -e
set -x

PLATFORM="$(uname -s | tr 'A-Z' 'a-z')"
function is_windows() {
  if [[ "${PLATFORM}" =~ (cygwin|mingw32|mingw64|msys)_nt* ]]; then
    true
  else
    false
  fi
}

if is_windows; then
  PIP_FILE_PREFIX="bazel-bin/build_pip_pkg.exe.runfiles/__main__/"
else
  PIP_FILE_PREFIX="bazel-bin/build_pip_pkg.runfiles/__main__/"
fi

PYTHON=python3
function main() {
  while [[ ! -z "${1}" ]]; do
    if [[ ${1} == "make" ]]; then
      echo "Using Makefile to build pip package."
      PIP_FILE_PREFIX=""
    elif [[ ${1} == "--python" ]]; then
      PYTHON=${2}
      shift
    else
      DEST=${1}
    fi
    shift
  done

  if [[ -z ${DEST} ]]; then
    echo "No destination dir provided"
    exit 1
  fi

  # Create the directory, then do dirname on a non-existent file inside it to
  # give us an absolute paths with tilde characters resolved to the destination
  # directory.
  mkdir -p ${DEST}
  if [[ ${PLATFORM} == "darwin" ]]; then
    DEST=$(pwd -P)/${DEST}
  else
    DEST=$(readlink -f "${DEST}")
  fi
  echo "=== destination directory: ${DEST}"

  TMPDIR=$(mktemp -d -t tmp.XXXXXXXXXX)

  echo $(date) : "=== Using tmpdir: ${TMPDIR}"

  echo "=== Copy TensorFlow Custom op files"

  cp ${PIP_FILE_PREFIX}setup.py "${TMPDIR}"
  cp ${PIP_FILE_PREFIX}MANIFEST.in "${TMPDIR}"
  cp ${PIP_FILE_PREFIX}LICENSE "${TMPDIR}"
  cp ${PIP_FILE_PREFIX}README.md "${TMPDIR}"
  cp ${PIP_FILE_PREFIX}requirements.txt "${TMPDIR}"
  rsync -avm -L --exclude='*.h' --exclude='*.cc' --exclude='*.o'              \
    --exclude='*.inc' --exclude='*.cpp' --exclude='*.a' --exclude='*.txt'     \
    --exclude='*.c' --exclude='*_test.py' --exclude='__pycache__/*'                           \
    ${PIP_FILE_PREFIX}tensorflow_nufft "${TMPDIR}"

  pushd ${TMPDIR}
  echo $(date) : "=== Building wheel"
  ${PYTHON} setup.py bdist_wheel > /dev/null

  if [[ "${PLATFORM}" == "linux" ]]; then
    echo $(date) : "=== Auditing wheel"
    ${PYTHON} -m auditwheel repair --plat manylinux2014_x86_64 dist/*linux_x86_64.whl -w dist/
    rm -rf dist/*linux_x86_64.whl
  fi

  cp dist/*.whl "${DEST}"
  popd
  rm -rf ${TMPDIR}
  echo $(date) : "=== Output wheel file is in: ${DEST}"
}

main "$@"
