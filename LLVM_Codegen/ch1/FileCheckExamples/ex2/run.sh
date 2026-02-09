#!/bin/bash

FileCheck --input-file input.txt check-file.txt --check-prefixes CHECK,SECOND
