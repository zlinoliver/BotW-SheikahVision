// The installer's prebuilt runtime supplies main(); this translation unit keeps the bundle target linkable.
static_assert(sizeof(void*) == 8);
