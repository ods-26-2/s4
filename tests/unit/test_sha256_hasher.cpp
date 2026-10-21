#include <cassert>
#include <iostream>
#include <string>
#include <vector>

#include "s4/infrastructure/hashing/sha256_hasher.hpp"

using namespace ods::s4;

std::vector<std::uint8_t> to_bytes(const std::string& value) {
    return std::vector<std::uint8_t>(value.begin(), value.end());
}

void assert_hash(const std::string& input, const std::string& expected) {
    const std::string actual = infrastructure::compute_sha256_hex(to_bytes(input));
    assert(actual == expected);
}

void test_deve_gerar_hash_sha256_conhecido_quando_entrada_vazia() {
    assert_hash("", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

void test_deve_gerar_hash_sha256_conhecido_quando_entrada_for_abc() {
    assert_hash("abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

void test_deve_gerar_hash_deterministico_quando_mesma_entrada() {
    const auto first = infrastructure::compute_sha256_hex(to_bytes("FRAME_00FRAME_01FRAME_02"));
    const auto second = infrastructure::compute_sha256_hex(to_bytes("FRAME_00FRAME_01FRAME_02"));
    assert(first == second);
    assert(first == "c5d94dd20a8429595a2d3232e6464179bba1e9c119d07d57e2494e56e230dd35");
}

void test_deve_gerar_hashes_diferentes_quando_entradas_diferem() {
    const auto first = infrastructure::compute_sha256_hex(to_bytes("a"));
    const auto second = infrastructure::compute_sha256_hex(to_bytes("b"));
    assert(first != second);
}

int main() {
    std::cout << "Running S4 SHA-256 Hasher Tests...\n";
    test_deve_gerar_hash_sha256_conhecido_quando_entrada_vazia();
    test_deve_gerar_hash_sha256_conhecido_quando_entrada_for_abc();
    test_deve_gerar_hash_deterministico_quando_mesma_entrada();
    test_deve_gerar_hashes_diferentes_quando_entradas_diferem();
    std::cout << "All S4 hasher tests passed successfully!\n";
    return 0;
}