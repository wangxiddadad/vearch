#!/usr/bin/env bash
VERSION=latest
ROOT=$(dirname ${PWD})

function get_version() {
  VEARCH_VERSION_MAJOR=$(cat ${ROOT}/VERSION | grep VEARCH_VERSION_MAJOR | awk -F' ' '{print $2}')
  VEARCH_VERSION_MINOR=$(cat ${ROOT}/VERSION | grep VEARCH_VERSION_MINOR | awk -F' ' '{print $2}')
  VEARCH_VERSION_PATCH=$(cat ${ROOT}/VERSION | grep VEARCH_VERSION_PATCH | awk -F' ' '{print $2}')

  VERSION="${VEARCH_VERSION_MAJOR}.${VEARCH_VERSION_MINOR}.${VEARCH_VERSION_PATCH}"
  echo "VERSION="${VERSION}
}

get_version

echo "Build compile Environment"
cd  env
docker build -t vearch/vearch-dev-env:latest .

cd ..
echo "Build Vearch Image"
docker build -f cloud/Dockerfile -t vearch/vearch:latest .

echo "Start service by all in one model"
cp config/config.toml .
nohup docker run --privileged -p 8817:8817 -p 9001:9001 -v $PWD/config.toml:/vearch/config.toml vearch/vearch:$VERSION all &

echo "good luck service is ready you can visit http://127.0.0.1:9001 to use it"
