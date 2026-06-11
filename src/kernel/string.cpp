extern "C" {

__attribute__((used)) void* memset(void* dest, int ch, unsigned long long count) {
    auto* d = static_cast<unsigned char*>(dest);
    for (unsigned long long i = 0; i < count; ++i) {
        d[i] = static_cast<unsigned char>(ch);
    }
    return dest;
}

__attribute__((used)) void* memcpy(void* dest, const void* src, unsigned long long count) {
    auto* d = static_cast<unsigned char*>(dest);
    const auto* s = static_cast<const unsigned char*>(src);
    for (unsigned long long i = 0; i < count; ++i) {
        d[i] = s[i];
    }
    return dest;
}

}
