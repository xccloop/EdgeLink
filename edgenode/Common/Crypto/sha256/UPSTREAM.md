Source: https://github.com/B-Con/crypto-algorithms
Commit: cfbde48414baacf51fc7c74f275190881f037d32
License: public domain, as stated in the upstream README.
Local portability change: memory.h replaced by standard C string.h for ARM newlib.
Local correctness change: convert input bytes to unsigned WORD before shifts to avoid signed-int left-shift undefined behavior.
