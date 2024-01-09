# thesis-nnlib
## A C++ framework for training and evaluation of deep neural networks in nvidia gpu-accelerated systems

### Example usage

Provides the `Neural` namespace plus `Neural::Network`, `Neural::Layers:Fc`, `Neural::Layers:Conv`, `Neural::Tensor4D`, `Neural::Shape4D` classes.

```cpp
#include "tensor.hpp"
#include "network.hpp"
#include "layer.hpp"

using Neural::Tensor4D;
using Neural::Shape4D;
using Neural::Network;

Tensor4D<double> train_data, valid_data, test_data; // assume initialized
Tensor4D<int> train_labels, valid_labels, test_labels; // assume initialized

// Initialize network with input data shape (channels, width, height). Batch size is left undefined.
Network testnet(train_data.shape());

// Add Conv Layer with activation
testnet.add_layer<Neural::Layers::Conv>(depth_conv1, "relu", filter_size_conv1, stride_conv1, padding_conv1);

// Add hidden FC layer with activation
testnet.add_layer<Neural::Layers::Fc>(num_hidden_nodes, "relu");

// Add output layer
testnet.add_layer<Neural::Layers::Fc>(num_outputs, "softmax");

// Set hyperparameters
int batch_size, max_epochs, max_steps_per_epoch;
double learning_rate;
bool accelerated;

// Train network with train and validation datasets
testnet.train(train_data, train_labels, valid_data, valid_labels, batch_size, accelerated, learning_rate, "CrossEntropy", max_epochs, max_steps_per_epoch);

// Evaluate network against test dataset and obtain precision and recall metrics
double precision_test, recall_test;
testnet.eval(test_data, test_labels, recall_test, precision_test);
```

## Installation
### Requirements
- Nvidia GPU
- Docker

### Step 1: Pull the docker image
```
docker pull sirmihawk/thesis:hpc22.11_build
```

### Step 2: Create a new docker container
The `--rm` option will create a container which will be auto-removed once the session is ended.
```
docker run -it --rm --gpus all sirmihawk/thesis:hpc22.11_build
```

### Step 3: Build the library and sample apps
Clone the repository inside the container:
```
git clone https://github.com/xbouroseu/thesis-nnlib
cd thesis-nnlib
```

Build the framework and samples with:
```
make all
```

Alternatively you can only build the library with:
```
make lib
``` 

and the samples with:
```
make examples
```

### Step 4: Run the sample MNIST training application
After building the library and the sample apps we can run one example application which is training and evaluating a Convolution Neural Network on the MNIST dataset.


```
cd apps/mnist_app
```

The following command is for log-level `info` and batch_size `x`. We can also choose to run either the gpu-accelerated version or the non-accelerated one.

For the gpu-accelerated version:
```
./build/mnist_acc info x
```

For the non-accelerated version:
```
./build/mnist_noacc info x
```
