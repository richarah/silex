#!/bin/bash
# One-time setup for contributors. Run after cloning.
set -e

git config core.hooksPath .githooks
echo "Git hooks configured (.githooks/)."
