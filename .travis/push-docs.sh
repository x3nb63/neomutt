#!/bin/sh

# exit early if any command fails
set -e

openssl aes-256-cbc -K $encrypted_3ed0348c37f7_key -iv $encrypted_3ed0348c37f7_iv -in .travis/doxygen.tar.enc -out .travis/doxygen.tar -d
tar xf .travis/doxygen.tar
head travis-deploy-doxygen
head travis-deploy-website

chmod 0400 travis-deploy-doxygen
chmod 0400 travis-deploy-website

eval "$(ssh-agent -s)"

ssh-add travis-deploy-doxygen
ssh-add travis-deploy-website

git clone --recursive https://github.com/neomutt/test-neomutt.github.io website

(
  cd website/code

  # Remove the old docs
  git rm -r --quiet ./*

  # Add the newly generated docs
  cp -r ../../html/* ./
  git commit --all -m "Update docs"

  git push origin master
)

git commit --all -m "Updated docs submodule"

git push origin master
