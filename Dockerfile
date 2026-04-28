FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

# Install dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    g++ \
    python3 \
    python3-pip \
    wget \
    curl \
    && rm -rf /var/lib/apt/lists/*

# Install Python packages
RUN pip3 install fastapi uvicorn

WORKDIR /app

# Copy all files
COPY . .

# Download httplib.h
RUN wget -q https://raw.githubusercontent.com/yhirose/cpp-httplib/master/httplib.h \
    -O src/httplib.h

# Build C++ engine
RUN mkdir -p build && cd build && \
    cmake .. -DCMAKE_BUILD_TYPE=Release && \
    make -j$(nproc) hft_engine

# Expose port
EXPOSE 7860

# Run FastAPI server
CMD ["python3", "app.py"]