Contributing guidelines
============

## Contributing to Intel® Extension for TensorFlow\*

We welcome community contributions to Intel® Extension for TensorFlow*. Before you begin writing code, it is important that you share your intention to contribute with the team, based on the type of contribution:

1. You want to submit a bug or propose a new feature.
    - Log a bug or feedback with an [issue](https://github.com/intel/intel-extension-for-tensorflow/issues).
    - Post about your intended feature in an [issue](https://github.com/intel/intel-extension-for-tensorflow/issues) for design and implementation approval.
2. You want to implement a bug-fix or feature for an issue.
    - Search for your issue in the [issue list](https://github.com/intel/intel-extension-for-tensorflow/issues).
    - Pick an issue and comment that you'd like to work on the bug-fix or feature.

* For bug-fix, please submit a Pull Request to project [github](https://github.com/intel/intel-extension-for-tensorflow/pulls).

  Ensure that you can build the product and run all the examples with your patch.
  Submit a [pull request](https://github.com/intel/intel-extension-for-tensorflow/pulls).


* For feature or significant changes require approval from Intel® Extension for TensorFlow\* maintainers before opening a pull request with such implementation. For that we use the `Request For Comments (RFC) process`, which consists of opening, discussing, and accepting (promoting) RFC pull requests.

  More information about the process can be found in the dedicated[`rfcs`](https://github.com/intel/intel-extension-for-tensorflow/tree/rfcs) branch.

**See also:** [Contributor Covenant](../../../CODE_OF_CONDUCT.md) code of conduct.

## Developing Intel® Extension for TensorFlow\*

Please refer to a full set of [instructions](../install/how_to_build.md) on installing Intel® Extension for TensorFlow\* from source.

## Tips and Debugging


## Unit testing

Intel® Extension for TensorFlow* provides python unit tests. 

### Python Unit Testing
* Python unit tests are located at `intel-extension-for-tensorflow/test`
```
test/
├── benchmark       # Benchmark unit tests
├── examples        # Unit test examples
├── llga            # LLGA unit tests
├── python          # Python API unit tests
├── sanity          # Sanity unit tests
└── tensorflow      # TensorFlow migrated unit tests
```

* Running single unit test:

```
python <path_to_python_unit_test>
```

* Running all the unit tests:

```
cd intel-extension-for-tensorflow
for ut in $(find test -name "*.py"); do
    python $ut
done
```

## Code style guide

Intel® Extension for TensorFlow* follows the same code style guide as  [TensorFlow code style guide](https://www.tensorflow.org/community/contribute/code_style) for Python and C++ code.

### Python coding style

Ensure the Python code changes are consistent with the [Python coding standards](https://github.com/google/styleguide/blob/gh-pages/pyguide.md).

Use `pylint` to check your Python changes. To install `pylint` and check a file with `pylint` against custom style definition from the Intel® Extension for TensorFlow* source code root directory:

```bash
pip install pylint

pylint --rcfile=intel-extension-for-tensorflow/.pylintrc myfile.py
```

### C++ coding style

The C++ code should conform to the [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html). Intel® Extension for TensorFlow* use both `clang-format` and `cpplint` to check C++ code style.

To install `clang-format` and check a file with `clang-format` against custom style definition from the Intel® Extension for TensorFlow* source code root directory:

```bash
apt-get install clang-format-12

# The -i option makes it inplace, by default formatted output is written to stdout
clang-format-12 -i -style=file <file>
```

To install `cpplint` and check a file with `cpplint` from the Intel® Extension for TensorFlow* source code root directory:

```bash
pip install cpplint

cpplint --filter=-legal/copyright --exclude=./third_party --recursive ./
```

Sometimes `cpplint` may report false positive errors, and you can comment code with `// NOLINT` or `// NOLINTNEXTLINE` to skip the line for check:

```c++
#include "mkl.h" // NOLINT(build/include_subdir)

// NOLINTNEXTLINE
#include "mkl.h"
```

### bazel style guide

[buildifier](https://github.com/bazelbuild/buildtools/tree/master/buildifier) is a tool for formatting bazel `BUILD` and `.bzl` files with a standard convention(`xxx.tpl` files are not supported).

To check bazel files manually:

```bash
# install go
wget https://golang.org/dl/go1.15.3.linux-amd64.tar.gz
sudo tar -C /usr/local -xzf go1.15.3.linux-amd64.tar.gz
export PATH=$PATH:/usr/local/go/bin

# install buildifier
go get github.com/bazelbuild/buildtools/buildifier
cd ~/go/src/github.com/bazelbuild/buildtools/buildifier
go install
export PATH=$PATH:/home/$USER/go/bin

# check style. DO NOT use buildifier -r dir/, it will skip files like xxx.BUILD
buildifier BUILD # or zzz.BUILD or xxx.bzl
```

### Documentation style guide

Intel® Extension for TensorFlow\* follows the same documentation style guide as [TensorFlow documentation style guide](https://www.tensorflow.org/community/contribute/docs_style).
