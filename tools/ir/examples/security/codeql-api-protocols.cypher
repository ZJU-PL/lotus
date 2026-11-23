# Native configuration/protocol checks; call presence alone proves no misuse.
# lotus-ir-pdg-query input.bc --analysis rules --cwe 295,326,327,611,732 --format json
# --rule cpp/insufficient-key-size --rule cpp/weak-cryptographic-algorithm
# --rule cpp/certificate-result-conflation --rule cpp/certificate-not-checked
# --rule cpp/dangerous-use-of-ssl-shutdown --rule cpp/curl-disabled-ssl
# --rule cpp/unsafe-dacl-security-descriptor --rule cpp/external-entity-expansion
# --rule cpp/wrong-use-of-the-umask --rule cpp/world-writable-file-creation
# --rule cpp/open-call-with-mode-argument --rule cpp/insecure-generation-of-filename
# Numeric option/mode facts use the target ABI; required source flag macros
# need -g -fdebug-macro. Do not substitute host constants for an unknown target.

MATCH (c:INST_FUNCALL) WHERE c.callee IN ['SSL_get_peer_certificate','SSL_get1_peer_certificate','SSL_get_verify_result','SSL_shutdown','SSL_free','curl_easy_setopt'] RETURN c.callee AS api, c.src AS location, c

MATCH (c:INST_FUNCALL) WHERE c.callee IN ['EVP_PKEY_CTX_set_rsa_keygen_bits','RSA_generate_key_ex','SetSecurityDescriptorDacl','xmlReadMemory','xmlReadFile','xmlCtxtUseOptions','umask','chmod','open','creat'] RETURN c.callee AS api, c.arg1_int AS argument_one, c.arg2_int AS argument_two, c.src AS location, c
