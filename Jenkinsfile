// Build no Jenkins (dextro-pipeline, stack cmake). coreMQTT, jsmn e Unity vêm
// dos submódulos (external/), então o agente não precisa de apt extra.
@Library('dextro-pipeline') _

dextroLib(
    stack: 'cmake',
    submodulos: true,
    ci: '''
        cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DDEXTRO_IOT_BUILD_TESTS=ON
        cmake --build build -j2
        (cd build && ctest --output-on-failure -L unit)

        # o parser recebe dado do fio: roda de novo sob ASan e UBSan
        cmake -S . -B build-san -G Ninja -DCMAKE_BUILD_TYPE=Debug -DDEXTRO_IOT_BUILD_TESTS=ON \
            -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
        cmake --build build-san -j2
        (cd build-san && ctest --output-on-failure -L unit)
    ''',
)
