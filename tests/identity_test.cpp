// Module, parameter, resource, and target-artifact keys are all SHA-256 over a
// length-prefixed record, so the primitive and the encoding are checked here
// once instead of separately per consumer.

#include <merlin/core/identity.hpp>

#include <cassert>
#include <string>

int main() {
  assert(merlin::Sha256Hex("") ==
         "e3b0c44298fc1c149afbf4c8996fb924"
         "27ae41e4649b934ca495991b7852b855");
  assert(merlin::Sha256Hex("abc") ==
         "ba7816bf8f01cfea414140de5dae2223"
         "b00361a396177a9cb410ff61f20015ad");
  // Longer than one 64-byte block, and exactly on the padding boundary.
  assert(merlin::Sha256Hex(std::string(56U, 'a')) ==
         "b35439a4ac6f0948b6d6f9e3c6af0f5f"
         "590ce20f1bde7090ef7970686ec6738a");

  assert(merlin::IsIdentity(merlin::MakeIdentity("record")));
  assert(!merlin::IsIdentity("record"));
  assert(!merlin::IsIdentity(merlin::Sha256Hex("record")));
  assert(!merlin::IsIdentity("sha256:" + std::string(64U, 'z')));

  // The length prefix is what stops one field's value from being read as the
  // next field, which is the only reason two records cannot collide by
  // rearranging the same characters.
  std::string separated;
  merlin::AppendIdentityField(separated, "name", "value");
  assert(separated == "name=5:value\n");

  std::string first;
  merlin::AppendIdentityField(first, "field", "a");
  merlin::AppendIdentityField(first, "field", "bc");
  std::string second;
  merlin::AppendIdentityField(second, "field", "ab");
  merlin::AppendIdentityField(second, "field", "c");
  assert(merlin::MakeIdentity(first) != merlin::MakeIdentity(second));

  std::string embedded;
  merlin::AppendIdentityField(embedded, "field", "a\nfield=1:b");
  std::string genuine;
  merlin::AppendIdentityField(genuine, "field", "a");
  merlin::AppendIdentityField(genuine, "field", "b");
  assert(merlin::MakeIdentity(embedded) != merlin::MakeIdentity(genuine));
}
