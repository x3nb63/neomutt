#!/bin/sh

# exit early if any command fails
set -e

git clone --recursive https://github.com/neomutt/neomutt.github.io

(
cd neomutt.github.io/code

# Remove the old docs
git rm -r --quiet ./*

# Add the newly generated docs
cp -r ../../html/* ./
git commit --all -m "Update docs"

# git push origin master
)

git commit --all -m "Updated docs submodule"
