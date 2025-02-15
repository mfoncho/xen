#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <err.h>

#include <xen-tools/common-macros.h>
#include <xen/asm/x86-vendors.h>
#include <xen/lib/x86/cpu-policy.h>
#include <xen/domctl.h>

static unsigned int nr_failures;
#define fail(fmt, ...)                          \
({                                              \
    nr_failures++;                              \
    (void)printf(fmt, ##__VA_ARGS__);           \
})

#define memdup(ptr)                             \
({                                              \
    typeof(*(ptr)) *p_ = (ptr);                 \
    void *n_ = malloc(sizeof(*p_));             \
                                                \
    if ( !n_ )                                  \
        err(1, "%s malloc failure", __func__);  \
                                                \
    memcpy(n_, p_, sizeof(*p_));                \
})

static void test_vendor_identification(void)
{
    static const struct test {
        union {
            char ident[12];
            struct {
                uint32_t b, d, c;
            };
        };
        unsigned int vendor;
    } tests[] = {
        /* The 1st entry should remain here to work around gcc bug 91667. */
        { { ""             }, X86_VENDOR_UNKNOWN },
        { { "            " }, X86_VENDOR_UNKNOWN },
        { { "xxxxxxxxxxxx" }, X86_VENDOR_UNKNOWN },

        { { "GenuineIntel" }, X86_VENDOR_INTEL },
        { { "AuthenticAMD" }, X86_VENDOR_AMD },
        { { "CentaurHauls" }, X86_VENDOR_CENTAUR },
        { { "  Shanghai  " }, X86_VENDOR_SHANGHAI },
        { { "HygonGenuine" }, X86_VENDOR_HYGON },
    };

    printf("Testing CPU vendor identification:\n");

    for ( size_t i = 0; i < ARRAY_SIZE(tests); ++i )
    {
        const struct test *t = &tests[i];
        unsigned int vendor = x86_cpuid_lookup_vendor(t->b, t->c, t->d);

        if ( vendor != t->vendor )
            fail("  Test '%.12s', expected vendor %u, got %u\n",
                 t->ident, t->vendor, vendor);
    }
}

static bool leaves_are_sorted(const xen_cpuid_leaf_t *leaves, unsigned int nr)
{
    for ( unsigned int i = 1; i < nr; ++i )
    {
        /* leaf index went backwards => not sorted. */
        if ( leaves[i - 1].leaf > leaves[i].leaf )
            return false;

        /* leaf index went forwards => ok */
        if ( leaves[i - 1].leaf < leaves[i].leaf )
            continue;

        /* leave index the same, subleaf didn't increase => not sorted. */
        if ( leaves[i - 1].subleaf >= leaves[i].subleaf )
            return false;
    }

    return true;
}

static bool msrs_are_sorted(const xen_msr_entry_t *entries, unsigned int nr)
{
    for ( unsigned int i = 1; i < nr; ++i )
        /* MSR index went backwards => not sorted. */
        if ( entries[i - 1].idx > entries[i].idx )
            return false;

    return true;
}

static void test_cpuid_current(void)
{
    struct cpu_policy p;
    xen_cpuid_leaf_t leaves[CPUID_MAX_SERIALISED_LEAVES];
    unsigned int nr = ARRAY_SIZE(leaves);
    int rc;

    printf("Testing CPUID on current CPU\n");

    x86_cpu_policy_fill_native(&p);

    rc = x86_cpuid_copy_to_buffer(&p, leaves, &nr);
    if ( rc != 0 )
        return fail("  Serialise, expected rc 0, got %d\n", rc);

    if ( !leaves_are_sorted(leaves, nr) )
        return fail("  Leaves not sorted\n");
}

static void test_cpuid_serialise_success(void)
{
    static const struct test {
        struct cpu_policy p;
        const char *name;
        unsigned int nr_leaves;
    } tests[] = {
        {
            .name = "empty policy",
            .nr_leaves = 4,
        },

        /* Leaf 4 serialisation stops at the first subleaf with type 0. */
        {
            .name = "empty leaf 4",
            .p = {
                .basic.max_leaf = 4,
            },
            .nr_leaves = 4 + 4,
        },
        {
            .name = "partial leaf 4",
            .p = {
                .basic.max_leaf = 4,
                .cache.subleaf[0].type = 1,
            },
            .nr_leaves = 4 + 4 + 1,
        },

        /* Leaf 7 serialisation stops at max_subleaf. */
        {
            .name = "empty leaf 7",
            .p = {
                .basic.max_leaf = 7,
            },
            .nr_leaves = 4 + 7,
        },
        {
            .name = "partial leaf 7",
            .p = {
                .basic.max_leaf = 7,
                .feat.max_subleaf = 1,
            },
            .nr_leaves = 4 + 7 + 1,
        },

        /* Leaf 0xb serialisation stops at the first subleaf with type 0. */
        {
            .name = "empty leaf 0xb",
            .p = {
                .basic.max_leaf = 0xb,
            },
            .nr_leaves = 4 + 0xb,
        },
        {
            .name = "partial leaf 0xb",
            .p = {
                .basic.max_leaf = 0xb,
                .topo.subleaf[0].type = 1,
            },
            .nr_leaves = 4 + 0xb + 1,
        },

        /*
         * Leaf 0xd serialisation automatically has two leaves, and stops the
         * highest bit set in {xcr0,xss}_{high,low}.
         */
        {
            .name = "empty leaf 0xd",
            .p = {
                .basic.max_leaf = 0xd,
            },
            .nr_leaves = 4 + 0xd + 1,
        },
        {
            .name = "partial 0xd",
            .p = {
                .basic.max_leaf = 0xd,
                .xstate.xcr0_low = 7,
            },
            .nr_leaves = 4 + 0xd + 1 + 1,
        },
    };

    printf("Testing CPUID serialise success:\n");

    for ( size_t i = 0; i < ARRAY_SIZE(tests); ++i )
    {
        const struct test *t = &tests[i];
        unsigned int nr = t->nr_leaves;
        xen_cpuid_leaf_t *leaves = malloc(nr * sizeof(*leaves));
        int rc;

        if ( !leaves )
            err(1, "%s() malloc failure", __func__);

        rc = x86_cpuid_copy_to_buffer(&t->p, leaves, &nr);

        if ( rc != 0 )
        {
            fail("  Test %s, expected rc 0, got %d\n",
                 t->name, rc);
            goto test_done;
        }

        if ( nr != t->nr_leaves )
        {
            fail("  Test %s, expected %u leaves, got %u\n",
                 t->name, t->nr_leaves, nr);
            goto test_done;
        }

        if ( !leaves_are_sorted(leaves, nr) )
        {
            fail("  Test %s, leaves not sorted\n",
                 t->name);
            goto test_done;
        }

    test_done:
        free(leaves);
    }
}

static void test_msr_serialise_success(void)
{
    static const struct test {
        struct cpu_policy p;
        const char *name;
        unsigned int nr_msrs;
    } tests[] = {
        {
            .name = "empty policy",
            .nr_msrs = MSR_MAX_SERIALISED_ENTRIES,
        },
    };

    printf("Testing MSR serialise success:\n");

    for ( size_t i = 0; i < ARRAY_SIZE(tests); ++i )
    {
        const struct test *t = &tests[i];
        unsigned int nr = t->nr_msrs;
        xen_msr_entry_t *msrs = malloc(nr * sizeof(*msrs));
        int rc;

        if ( !msrs )
            err(1, "%s() malloc failure", __func__);

        rc = x86_msr_copy_to_buffer(&t->p, msrs, &nr);

        if ( rc != 0 )
        {
            fail("  Test %s, expected rc 0, got %d\n",
                 t->name, rc);
            goto test_done;
        }

        if ( nr != t->nr_msrs )
        {
            fail("  Test %s, expected %u msrs, got %u\n",
                 t->name, t->nr_msrs, nr);
            goto test_done;
        }

        if ( !msrs_are_sorted(msrs, nr) )
        {
            fail("  Test %s, MSR entries not sorted\n",
                 t->name);
            goto test_done;
        }

    test_done:
        free(msrs);
    }
}

static void test_cpuid_deserialise_failure(void)
{
    static const struct test {
        const char *name;
        xen_cpuid_leaf_t leaf;
    } tests[] = {
        {
            .name = "incorrect basic subleaf",
            .leaf = { .leaf = 0, .subleaf = 0 },
        },
        {
            .name = "incorrect hv1 subleaf",
            .leaf = { .leaf = 0x40000000, .subleaf = 0 },
        },
        {
            .name = "incorrect hv2 subleaf",
            .leaf = { .leaf = 0x40000100, .subleaf = 0 },
        },
        {
            .name = "incorrect extd subleaf",
            .leaf = { .leaf = 0x80000000, .subleaf = 0 },
        },
        {
            .name = "OoB basic leaf",
            .leaf = { .leaf = CPUID_GUEST_NR_BASIC },
        },
        {
            .name = "OoB cache leaf",
            .leaf = { .leaf = 0x4, .subleaf = CPUID_GUEST_NR_CACHE },
        },
        {
            .name = "OoB feat leaf",
            .leaf = { .leaf = 0x7, .subleaf = CPUID_GUEST_NR_FEAT },
        },
        {
            .name = "OoB topo leaf",
            .leaf = { .leaf = 0xb, .subleaf = CPUID_GUEST_NR_TOPO },
        },
        {
            .name = "OoB xstate leaf",
            .leaf = { .leaf = 0xd, .subleaf = CPUID_GUEST_NR_XSTATE },
        },
        {
            .name = "OoB extd leaf",
            .leaf = { .leaf = 0x80000000 | CPUID_GUEST_NR_EXTD },
        },
    };

    printf("Testing CPUID deserialise failure:\n");

    for ( size_t i = 0; i < ARRAY_SIZE(tests); ++i )
    {
        const struct test *t = &tests[i];
        uint32_t err_leaf, err_subleaf;
        int rc;

        /* No writes should occur.  Use NULL to catch errors. */
        rc = x86_cpuid_copy_from_buffer(NULL, &t->leaf, 1,
                                        &err_leaf, &err_subleaf);

        if ( rc != -ERANGE )
        {
            fail("  Test %s, expected rc %d, got %d\n",
                 t->name, -ERANGE, rc);
            continue;
        }

        if ( err_leaf != t->leaf.leaf || err_subleaf != t->leaf.subleaf )
        {
            fail("  Test %s, expected err %08x:%08x, got %08x:%08x\n",
                 t->name, t->leaf.leaf, t->leaf.subleaf,
                 err_leaf, err_subleaf);
            continue;
        }
    }
}

static void test_msr_deserialise_failure(void)
{
    static const struct test {
        const char *name;
        xen_msr_entry_t msr;
        int rc;
    } tests[] = {
        {
            .name = "bad msr index",
            .msr = { .idx = 0xdeadc0de },
            .rc = -ERANGE,
        },
        {
            .name = "nonzero flags",
            .msr = { .idx = 0xce, .flags = 1 },
            .rc = -EINVAL,
        },
        {
            .name = "truncated val",
            .msr = { .idx = 0xce, .val = ~0ull },
            .rc = -EOVERFLOW,
        },
        {
            .name = "truncated val",
            .msr = { .idx = 0x10a, .val = ~0ull },
            .rc = -EOVERFLOW,
        },
    };

    printf("Testing MSR deserialise failure:\n");

    for ( size_t i = 0; i < ARRAY_SIZE(tests); ++i )
    {
        const struct test *t = &tests[i];
        uint32_t err_msr;
        int rc;

        /* No writes should occur.  Use NULL to catch errors. */
        rc = x86_msr_copy_from_buffer(NULL, &t->msr, 1, &err_msr);

        if ( rc != t->rc )
        {
            fail("  Test %s, expected rc %d, got %d\n",
                 t->name, t->rc, rc);
            continue;
        }

        if ( err_msr != t->msr.idx )
        {
            fail("  Test %s, expected err_msr %#x, got %#x\n",
                 t->name, t->msr.idx, err_msr);
            continue;
        }
    }
}

static void test_cpuid_out_of_range_clearing(void)
{
    static const struct test {
        const char *name;
        unsigned int nr_markers;
        struct cpu_policy p;
    } tests[] = {
        {
            .name = "basic",
            .nr_markers = 1,
            .p = {
                /* Retains marker in leaf 0.  Clears others. */
                .basic.max_leaf = 0,
                .basic.vendor_ebx = 0xc2,

                .basic.raw_fms = 0xc2,
                .cache.raw[0].a = 0xc2,
                .feat.raw[0].a = 0xc2,
                .topo.raw[0].a = 0xc2,
                .xstate.raw[0].a = 0xc2,
                .xstate.raw[1].a = 0xc2,
            },
        },
        {
            .name = "cache",
            .nr_markers = 1,
            .p = {
                /* Retains marker in subleaf 0.  Clears others. */
                .basic.max_leaf = 4,
                .cache.raw[0] = { .a = 1, .b = 0xc2 },

                .cache.raw[1].b = 0xc2,
                .feat.raw[0].a = 0xc2,
                .topo.raw[0].a = 0xc2,
                .xstate.raw[0].a = 0xc2,
                .xstate.raw[1].a = 0xc2,
            },
        },
        {
            .name = "feat",
            .nr_markers = 1,
            .p = {
                /* Retains marker in subleaf 0.  Clears others. */
                .basic.max_leaf = 7,
                .feat.raw[0].b = 0xc2,

                .feat.raw[1].b = 0xc2,
                .topo.raw[0].a = 0xc2,
                .xstate.raw[0].a = 0xc2,
                .xstate.raw[1].a = 0xc2,
            },
        },
        {
            .name = "topo",
            .nr_markers = 1,
            .p = {
                /* Retains marker in subleaf 0.  Clears others. */
                .basic.max_leaf = 0xb,
                .topo.raw[0] = { .b = 0xc2, .c = 0x0100 },

                .topo.raw[1].b = 0xc2,
                .xstate.raw[0].a = 0xc2,
                .xstate.raw[1].a = 0xc2,
            },
        },
        {
            .name = "xstate x87",
            .nr_markers = 2,
            .p = {
                /* First two subleaves always valid.  Others cleared. */
                .basic.max_leaf = 0xd,
                .xstate.raw[0].a = 1,
                .xstate.raw[0].b = 0xc2,
                .xstate.raw[1].b = 0xc2,

                .xstate.raw[2].b = 0xc2,
                .xstate.raw[3].b = 0xc2,
            },
        },
        {
            .name = "xstate sse",
            .nr_markers = 2,
            .p = {
                /* First two subleaves always valid.  Others cleared. */
                .basic.max_leaf = 0xd,
                .xstate.raw[0].a = 2,
                .xstate.raw[0].b = 0xc2,
                .xstate.raw[1].b = 0xc2,

                .xstate.raw[2].b = 0xc2,
                .xstate.raw[3].b = 0xc2,
            },
        },
        {
            .name = "xstate avx",
            .nr_markers = 3,
            .p = {
                /* Third subleaf also valid.  Others cleared. */
                .basic.max_leaf = 0xd,
                .xstate.raw[0].a = 7,
                .xstate.raw[0].b = 0xc2,
                .xstate.raw[1].b = 0xc2,
                .xstate.raw[2].b = 0xc2,

                .xstate.raw[3].b = 0xc2,
            },
        },
        {
            .name = "extd",
            .nr_markers = 1,
            .p = {
                /* Retains marker in leaf 0.  Clears others. */
                .extd.max_leaf = 0,
                .extd.vendor_ebx = 0xc2,

                .extd.raw_fms = 0xc2,
            },
        },
    };

    printf("Testing CPUID out-of-range clearing:\n");

    for ( size_t i = 0; i < ARRAY_SIZE(tests); ++i )
    {
        const struct test *t = &tests[i];
        struct cpu_policy *p = memdup(&t->p);
        void *ptr;
        unsigned int nr_markers;

        x86_cpu_policy_clear_out_of_range_leaves(p);

        /* Count the number of 0xc2's still remaining. */
        for ( ptr = p, nr_markers = 0;
              (ptr = memchr(ptr, 0xc2, (void *)p + sizeof(*p) - ptr));
              ptr++, nr_markers++ )
            ;

        if ( nr_markers != t->nr_markers )
             fail("  Test %s fail - expected %u markers, got %u\n",
                  t->name, t->nr_markers, nr_markers);

        free(p);
    }
}

static void test_is_compatible_success(void)
{
    static struct test {
        const char *name;
        struct cpu_policy host, guest;
    } tests[] = {
        {
            .name = "Host CPUID faulting, Guest not",
            .host = {
                .platform_info.cpuid_faulting = true,
            },
        },
        {
            .name = "Host CPUID faulting, Guest wanted",
            .host = {
                .platform_info.cpuid_faulting = true,
            },
            .guest = {
                .platform_info.cpuid_faulting = true,
            },
        },
    };
    struct cpu_policy_errors no_errors = INIT_CPU_POLICY_ERRORS;

    printf("Testing policy compatibility success:\n");

    for ( size_t i = 0; i < ARRAY_SIZE(tests); ++i )
    {
        struct test *t = &tests[i];
        struct cpu_policy_errors e;
        int res = x86_cpu_policies_are_compatible(&t->host, &t->guest, &e);

        /* Check the expected error output. */
        if ( res != 0 || memcmp(&no_errors, &e, sizeof(no_errors)) )
            fail("  Test '%s' expected no errors\n"
                 "    got res %d { leaf %08x, subleaf %08x, msr %08x }\n",
                 t->name, res, e.leaf, e.subleaf, e.msr);
    }
}

static void test_is_compatible_failure(void)
{
    static struct test {
        const char *name;
        struct cpu_policy host, guest;
        struct cpu_policy_errors e;
    } tests[] = {
        {
            .name = "Host basic.max_leaf out of range",
            .guest.basic.max_leaf = 1,
            .e = { 0, -1, -1 },
        },
        {
            .name = "Host extd.max_leaf out of range",
            .guest.extd.max_leaf = 1,
            .e = { 0x80000000, -1, -1 },
        },
        {
            .name = "Host no CPUID faulting, Guest wanted",
            .guest = {
                .platform_info.cpuid_faulting = true,
            },
            .e = { -1, -1, 0xce },
        },
    };

    printf("Testing policy compatibility failure:\n");

    for ( size_t i = 0; i < ARRAY_SIZE(tests); ++i )
    {
        struct test *t = &tests[i];
        struct cpu_policy_errors e;
        int res = x86_cpu_policies_are_compatible(&t->host, &t->guest, &e);

        /* Check the expected error output. */
        if ( res == 0 || memcmp(&t->e, &e, sizeof(t->e)) )
            fail("  Test '%s' res %d\n"
                 "    expected { leaf %08x, subleaf %08x, msr %08x }\n"
                 "    got      { leaf %08x, subleaf %08x, msr %08x }\n",
                 t->name, res,
                 t->e.leaf, t->e.subleaf, t->e.msr,
                 e.leaf, e.subleaf, e.msr);
    }
}

int main(int argc, char **argv)
{
    printf("CPU Policy unit tests\n");

    test_vendor_identification();

    test_cpuid_current();
    test_cpuid_serialise_success();
    test_cpuid_deserialise_failure();
    test_cpuid_out_of_range_clearing();

    test_msr_serialise_success();
    test_msr_deserialise_failure();

    test_is_compatible_success();
    test_is_compatible_failure();

    if ( nr_failures )
        printf("Done: %u failures\n", nr_failures);
    else
        printf("Done: all ok\n");

    return !!nr_failures;
}
