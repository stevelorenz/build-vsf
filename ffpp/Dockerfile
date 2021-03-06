#
# This image is used for the development purpose.
#

FROM ubuntu:20.04

# Install dependencies for DPDK and xdp-tools.
RUN apt-get update && \
	DEBIAN_FRONTEND="noninteractive" apt-get install -y git wget build-essential m4 pkg-config \
	python3 meson cmake ninja-build libnuma-dev libpcap-dev libelf-dev clang llvm gcc-multilib \
	linux-tools-common python3-pyelftools python-pyelftools

WORKDIR /opt/

# Build xdp-tools from source using git.
# The reason to use git instead of downloading a tarball or deb package is that the
# xdp-tools project is currently still under heavy development without a quick release of stable deb packages.
# xdp-tools is based on the libbpf (the forked one in xdp project).
# It's easy to use git submodule to fetch the exact version used by xdp-tools and make sure it compiles.
ENV XDP_TOOLS_VER="v1.2.0"
ENV XDP_TOOLS_DIR="/opt/xdp-tools"
RUN mkdir -p ${XDP_TOOLS_DIR} && \
	git clone https://github.com/xdp-project/xdp-tools && \
    cd ./xdp-tools && git checkout -b ${XDP_TOOLS_VER} ${XDP_TOOLS_VER} && \
	git submodule init && git submodule update && \
	./configure && make && make install && \
	cd ./lib/libbpf/src && make install

# Let the build system and linker to find the libbpf.so
ENV PKG_CONFIG_PATH=${PKG_CONFIG_PATH}:/usr/lib64/pkgconfig
ENV LD_LIBRARY_PATH=${LD_LIBRARY_PATH}:/usr/lib64

# Build DPDK from source.
# The deb package (of Ubuntu 20.04) is not used currently since it does not
# contain AF_XDP PMD. Will use the deb package when AF_XDP is fully supported.
ENV DPDK_VER="21.05"
ENV DPDK_BUILD_OPTION="-Dtests=false"
ENV RTE_SDK="/opt/dpdk"
ENV RTE_TARGET="x86_64-native-linuxapp-gcc"
RUN mkdir -p ${RTE_SDK} && \
    wget http://fast.dpdk.org/rel/dpdk-${DPDK_VER}.tar.xz && \
    tar -xJf dpdk-${DPDK_VER}.tar.xz -C ./dpdk --strip-components 1 && \
    cd ./dpdk && meson ${DPDK_BUILD_OPTION} build && cd build && \
    ninja && ninja install && ldconfig && \
	rm -rf /opt/dpdk-${DPDK_VER}.tar.xz

# Install FFPP dependencies. 
RUN debian_frontend="noninteractive" apt-get install -y \
    libczmq-dev libjansson-dev \
    libcpufreq-dev \
	python3-pybind11 python3-pip python3-zmq python3-dev python3-numpy

# Install FFPP dependencies that are currenly under test.
RUN debian_frontend="noninteractive" apt-get install -y \
    libtins-dev
RUN pip3 install -q docker

# Install FFPP dev tools. These packages are not needed in deployment.
RUN debian_frontend="noninteractive" apt-get install -y \
	gcovr googletest google-mock \
    bash-completion cppcheck clang-tidy net-tools iproute2 iputils-ping tcpdump

# Build and install the FFPP library.
ENV FFPP_PATH /ffpp
RUN mkdir -p ${FFPP_PATH}
COPY . ${FFPP_PATH}
WORKDIR ${FFPP_PATH}
RUN make release && make install
ENV PKG_CONFIG_PATH=${PKG_CONFIG_PATH}:/usr/local/lib/x86_64-linux-gnu/pkgconfig
ENV LD_LIBRARY_PATH=${LD_LIBRARY_PATH}:/usr/local/lib/x86_64-linux-gnu

# APT cleanup.
RUN rm -rf /var/lib/apt/lists /var/cache/apt/archives

WORKDIR ${FFPP_PATH}

CMD ["bash"]
