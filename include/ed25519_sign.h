#ifndef PROGRESSIV_ED25519_SIGN_H
#define PROGRESSIV_ED25519_SIGN_H

#include <string>
#include <vector>

std::vector<unsigned char> ed25519_seed_from_pkcs8_der(const std::vector<unsigned char>& der);
std::string ed25519_sign_b64(const std::vector<unsigned char>& seed, const std::string& message);

#endif
