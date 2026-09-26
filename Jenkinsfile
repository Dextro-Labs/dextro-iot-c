// Build no Jenkins (dextro-pipeline). O GitHub Actions saiu: todo build roda
// no Jenkins desde 26/09/2026.
@Library('dextro-pipeline') _

dextroLib(stack: 'cmake', build: 'gcc -c src/dextro_iot.c -I include -o dextro_iot.o && ar rcs libdextro_iot.a dextro_iot.o')
