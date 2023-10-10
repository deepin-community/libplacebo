#include "tests.h"
#include "colorspace.h"

int main()
{
    for (enum pl_color_system sys = 0; sys < PL_COLOR_SYSTEM_COUNT; sys++) {
        bool ycbcr = sys >= PL_COLOR_SYSTEM_BT_601 && sys <= PL_COLOR_SYSTEM_YCGCO;
        REQUIRE_CMP(ycbcr, ==, pl_color_system_is_ycbcr_like(sys), "d");
    }

    for (enum pl_color_transfer trc = 0; trc < PL_COLOR_TRC_COUNT; trc++) {
        bool hdr = trc >= PL_COLOR_TRC_PQ && trc <= PL_COLOR_TRC_S_LOG2;
        REQUIRE_CMP(hdr, ==, pl_color_transfer_is_hdr(trc), "d");
        REQUIRE_CMP(pl_color_transfer_nominal_peak(trc), >=, 1.0, "f");
    }

    float pq_peak = pl_color_transfer_nominal_peak(PL_COLOR_TRC_PQ);
    REQUIRE_FEQ(PL_COLOR_SDR_WHITE * pq_peak, 10000, 1e-7);

    struct pl_color_repr tv_repr = {
        .sys       = PL_COLOR_SYSTEM_BT_709,
        .levels    = PL_COLOR_LEVELS_LIMITED,
    };

    struct pl_color_repr pc_repr = {
        .sys       = PL_COLOR_SYSTEM_RGB,
        .levels    = PL_COLOR_LEVELS_FULL,
    };

    // Ensure this is a no-op for bits == bits
    for (int bits = 1; bits <= 16; bits++) {
        tv_repr.bits.color_depth = tv_repr.bits.sample_depth = bits;
        pc_repr.bits.color_depth = pc_repr.bits.sample_depth = bits;
        REQUIRE_FEQ(pl_color_repr_normalize(&tv_repr), 1.0, 1e-7);
        REQUIRE_FEQ(pl_color_repr_normalize(&pc_repr), 1.0, 1e-7);
    }

    tv_repr.bits.color_depth  = 8;
    tv_repr.bits.sample_depth = 10;
    float tv8to10 = pl_color_repr_normalize(&tv_repr);

    tv_repr.bits.color_depth  = 8;
    tv_repr.bits.sample_depth = 12;
    float tv8to12 = pl_color_repr_normalize(&tv_repr);

    // Simulate the effect of GPU texture sampling on UNORM texture
    REQUIRE_FEQ(tv8to10 * 16 /1023.,  64/1023., 1e-7); // black
    REQUIRE_FEQ(tv8to10 * 235/1023., 940/1023., 1e-7); // nominal white
    REQUIRE_FEQ(tv8to10 * 128/1023., 512/1023., 1e-7); // achromatic
    REQUIRE_FEQ(tv8to10 * 240/1023., 960/1023., 1e-7); // nominal chroma peak

    REQUIRE_FEQ(tv8to12 * 16 /4095., 256 /4095., 1e-7); // black
    REQUIRE_FEQ(tv8to12 * 235/4095., 3760/4095., 1e-7); // nominal white
    REQUIRE_FEQ(tv8to12 * 128/4095., 2048/4095., 1e-7); // achromatic
    REQUIRE_FEQ(tv8to12 * 240/4095., 3840/4095., 1e-7); // nominal chroma peak

    // Ensure lavc's xyz12 is handled correctly
    struct pl_color_repr xyz12 = {
        .sys    = PL_COLOR_SYSTEM_XYZ,
        .levels = PL_COLOR_LEVELS_UNKNOWN,
        .bits   = {
            .sample_depth = 16,
            .color_depth  = 12,
            .bit_shift    = 4,
        },
    };

    float xyz = pl_color_repr_normalize(&xyz12);
    REQUIRE_FEQ(xyz * (4095 << 4), 65535, 1e-7);

    // Assume we uploaded a 10-bit source directly (unshifted) as a 16-bit
    // texture. This texture multiplication factor should make it behave as if
    // it was uploaded as a 10-bit texture instead.
    pc_repr.bits.color_depth = 10;
    pc_repr.bits.sample_depth = 16;
    float pc10to16 = pl_color_repr_normalize(&pc_repr);
    REQUIRE_FEQ(pc10to16 * 1000/65535., 1000/1023., 1e-7);

    const struct pl_raw_primaries *bt709, *bt2020, *dcip3;
    bt709 = pl_raw_primaries_get(PL_COLOR_PRIM_BT_709);
    bt2020 = pl_raw_primaries_get(PL_COLOR_PRIM_BT_2020);
    dcip3 = pl_raw_primaries_get(PL_COLOR_PRIM_DCI_P3);
    REQUIRE(pl_primaries_superset(bt2020, bt709));
    REQUIRE(!pl_primaries_superset(bt2020, dcip3)); // small region doesn't overlap
    REQUIRE(pl_primaries_superset(dcip3, bt709));
    REQUIRE(!pl_primaries_superset(bt709, bt2020));
    REQUIRE(pl_primaries_compatible(bt2020, bt2020));
    REQUIRE(pl_primaries_compatible(bt2020, bt709));
    REQUIRE(pl_primaries_compatible(bt709, bt2020));
    REQUIRE(pl_primaries_compatible(bt2020, dcip3));
    REQUIRE(pl_primaries_compatible(bt709, dcip3));

    struct pl_raw_primaries bt709_2020 = pl_primaries_clip(bt709, bt2020);
    struct pl_raw_primaries bt2020_709 = pl_primaries_clip(bt2020, bt709);
    REQUIRE(pl_raw_primaries_similar(&bt709_2020, bt709));
    REQUIRE(pl_raw_primaries_similar(&bt2020_709, bt709));

    struct pl_raw_primaries dcip3_bt2020 = pl_primaries_clip(dcip3, bt2020);
    struct pl_raw_primaries dcip3_bt709  = pl_primaries_clip(dcip3, bt709);
    REQUIRE(pl_primaries_superset(dcip3,  &dcip3_bt2020));
    REQUIRE(pl_primaries_superset(dcip3,  &dcip3_bt709));
    REQUIRE(pl_primaries_superset(bt2020, &dcip3_bt2020));
    REQUIRE(pl_primaries_superset(bt709,  &dcip3_bt709));

    pl_matrix3x3 rgb2xyz, rgb2xyz_;
    rgb2xyz = rgb2xyz_ = pl_get_rgb2xyz_matrix(bt709);
    pl_matrix3x3_invert(&rgb2xyz_);
    pl_matrix3x3_invert(&rgb2xyz_);

    // Make sure the double-inversion round trips
    for (int y = 0; y < 3; y++) {
        for (int x = 0; x < 3; x++)
            REQUIRE_FEQ(rgb2xyz.m[y][x], rgb2xyz_.m[y][x], 1e-6);
    }

    // Make sure mapping the spectral RGB colors (i.e. the matrix rows) matches
    // our original primaries
    float Y = rgb2xyz.m[1][0];
    REQUIRE_FEQ(rgb2xyz.m[0][0], pl_cie_X(bt709->red) * Y, 1e-7);
    REQUIRE_FEQ(rgb2xyz.m[2][0], pl_cie_Z(bt709->red) * Y, 1e-7);
    Y = rgb2xyz.m[1][1];
    REQUIRE_FEQ(rgb2xyz.m[0][1], pl_cie_X(bt709->green) * Y, 1e-7);
    REQUIRE_FEQ(rgb2xyz.m[2][1], pl_cie_Z(bt709->green) * Y, 1e-7);
    Y = rgb2xyz.m[1][2];
    REQUIRE_FEQ(rgb2xyz.m[0][2], pl_cie_X(bt709->blue) * Y, 1e-7);
    REQUIRE_FEQ(rgb2xyz.m[2][2], pl_cie_Z(bt709->blue) * Y, 1e-7);

    // Make sure the gamut mapping round-trips
    pl_matrix3x3 bt709_bt2020, bt2020_bt709;
    bt709_bt2020 = pl_get_color_mapping_matrix(bt709, bt2020, PL_INTENT_RELATIVE_COLORIMETRIC);
    bt2020_bt709 = pl_get_color_mapping_matrix(bt2020, bt709, PL_INTENT_RELATIVE_COLORIMETRIC);
    for (int n = 0; n < 10; n++) {
        float vec[3] = { RANDOM, RANDOM, RANDOM };
        float dst[3] = { vec[0],    vec[1],    vec[2]    };
        pl_matrix3x3_apply(&bt709_bt2020, dst);
        pl_matrix3x3_apply(&bt2020_bt709, dst);
        for (int i = 0; i < 3; i++)
            REQUIRE_FEQ(dst[i], vec[i], 1e-6);
    }

    // Ensure the decoding matrix round-trips to white/black
    for (enum pl_color_system sys = 0; sys < PL_COLOR_SYSTEM_COUNT; sys++) {
        if (!pl_color_system_is_linear(sys))
            continue;

        printf("testing color system %u\n", (unsigned) sys);
        struct pl_color_repr repr = {
            .levels = PL_COLOR_LEVELS_LIMITED,
            .sys = sys,
            .bits = {
                // synthetic test
                .color_depth = 8,
                .sample_depth = 10,
            },
        };

        float scale = pl_color_repr_normalize(&repr);
        pl_transform3x3 yuv2rgb = pl_color_repr_decode(&repr, NULL);
        pl_matrix3x3_scale(&yuv2rgb.mat, scale);

        static const float white_ycbcr[3] = { 235/1023., 128/1023., 128/1023. };
        static const float black_ycbcr[3] = {  16/1023., 128/1023., 128/1023. };
        static const float white_other[3] = { 235/1023., 235/1023., 235/1023. };
        static const float black_other[3] = {  16/1023.,  16/1023.,  16/1023. };

        float white[3], black[3];
        for (int i = 0; i < 3; i++) {
            if (pl_color_system_is_ycbcr_like(sys)) {
                white[i] = white_ycbcr[i];
                black[i] = black_ycbcr[i];
            } else {
                white[i] = white_other[i];
                black[i] = black_other[i];
            }
        }

        pl_transform3x3_apply(&yuv2rgb, white);
        REQUIRE_FEQ(white[0], 1.0, 1e-6);
        REQUIRE_FEQ(white[1], 1.0, 1e-6);
        REQUIRE_FEQ(white[2], 1.0, 1e-6);

        pl_transform3x3_apply(&yuv2rgb, black);
        REQUIRE_FEQ(black[0], 0.0, 1e-6);
        REQUIRE_FEQ(black[1], 0.0, 1e-6);
        REQUIRE_FEQ(black[2], 0.0, 1e-6);
    }

    // Make sure chromatic adaptation works
    struct pl_raw_primaries bt709_d50;
    bt709_d50 = *pl_raw_primaries_get(PL_COLOR_PRIM_BT_709);
    bt709_d50.white = (struct pl_cie_xy) { 0.34567, 0.35850 };

    pl_matrix3x3 d50_d65;
    d50_d65 = pl_get_color_mapping_matrix(&bt709_d50, bt709, PL_INTENT_RELATIVE_COLORIMETRIC);

    float white[3] = { 1.0, 1.0, 1.0 };
    pl_matrix3x3_apply(&d50_d65, white);
    REQUIRE_FEQ(white[0], 1.0, 1e-6);
    REQUIRE_FEQ(white[1], 1.0, 1e-6);
    REQUIRE_FEQ(white[2], 1.0, 1e-6);

    // Simulate a typical 10-bit YCbCr -> 16 bit texture conversion
    tv_repr.bits.color_depth  = 10;
    tv_repr.bits.sample_depth = 16;
    pl_transform3x3 yuv2rgb;
    yuv2rgb = pl_color_repr_decode(&tv_repr, NULL);
    float test[3] = { 575/65535., 336/65535., 640/65535. };
    pl_transform3x3_apply(&yuv2rgb, test);
    REQUIRE_FEQ(test[0], 0.808305, 1e-6);
    REQUIRE_FEQ(test[1], 0.553254, 1e-6);
    REQUIRE_FEQ(test[2], 0.218841, 1e-6);

    // DVD
    REQUIRE_CMP(pl_color_system_guess_ycbcr(720, 480), ==, PL_COLOR_SYSTEM_BT_601, "u");
    REQUIRE_CMP(pl_color_system_guess_ycbcr(720, 576), ==, PL_COLOR_SYSTEM_BT_601, "u");
    REQUIRE_CMP(pl_color_primaries_guess(720, 576), ==, PL_COLOR_PRIM_BT_601_625, "u");
    REQUIRE_CMP(pl_color_primaries_guess(720, 480), ==, PL_COLOR_PRIM_BT_601_525, "u");
    // PAL 16:9
    REQUIRE_CMP(pl_color_system_guess_ycbcr(1024, 576), ==, PL_COLOR_SYSTEM_BT_601, "u");
    REQUIRE_CMP(pl_color_primaries_guess(1024, 576), ==, PL_COLOR_PRIM_BT_601_625, "u");
    // HD
    REQUIRE_CMP(pl_color_system_guess_ycbcr(1280, 720),  ==, PL_COLOR_SYSTEM_BT_709, "u");
    REQUIRE_CMP(pl_color_system_guess_ycbcr(1920, 1080), ==, PL_COLOR_SYSTEM_BT_709, "u");
    REQUIRE_CMP(pl_color_primaries_guess(1280, 720),  ==, PL_COLOR_PRIM_BT_709, "u");
    REQUIRE_CMP(pl_color_primaries_guess(1920, 1080), ==, PL_COLOR_PRIM_BT_709, "u");

    // Odd/weird videos
    REQUIRE_CMP(pl_color_primaries_guess(2000, 576), ==, PL_COLOR_PRIM_BT_709, "u");
    REQUIRE_CMP(pl_color_primaries_guess(200, 200),  ==, PL_COLOR_PRIM_BT_709, "u");

    REQUIRE(pl_color_repr_equal(&pl_color_repr_sdtv, &pl_color_repr_sdtv));
    REQUIRE(!pl_color_repr_equal(&pl_color_repr_sdtv, &pl_color_repr_hdtv));

    struct pl_color_repr repr = pl_color_repr_unknown;
    pl_color_repr_merge(&repr, &pl_color_repr_uhdtv);
    REQUIRE(pl_color_repr_equal(&repr, &pl_color_repr_uhdtv));

    REQUIRE(!pl_color_primaries_is_wide_gamut(PL_COLOR_PRIM_UNKNOWN));
    REQUIRE(!pl_color_primaries_is_wide_gamut(PL_COLOR_PRIM_BT_601_525));
    REQUIRE(!pl_color_primaries_is_wide_gamut(PL_COLOR_PRIM_BT_601_625));
    REQUIRE(!pl_color_primaries_is_wide_gamut(PL_COLOR_PRIM_BT_709));
    REQUIRE(!pl_color_primaries_is_wide_gamut(PL_COLOR_PRIM_BT_470M));
    REQUIRE(pl_color_primaries_is_wide_gamut(PL_COLOR_PRIM_BT_2020));
    REQUIRE(pl_color_primaries_is_wide_gamut(PL_COLOR_PRIM_APPLE));
    REQUIRE(pl_color_primaries_is_wide_gamut(PL_COLOR_PRIM_ADOBE));
    REQUIRE(pl_color_primaries_is_wide_gamut(PL_COLOR_PRIM_PRO_PHOTO));
    REQUIRE(pl_color_primaries_is_wide_gamut(PL_COLOR_PRIM_CIE_1931));
    REQUIRE(pl_color_primaries_is_wide_gamut(PL_COLOR_PRIM_DCI_P3));
    REQUIRE(pl_color_primaries_is_wide_gamut(PL_COLOR_PRIM_DISPLAY_P3));
    REQUIRE(pl_color_primaries_is_wide_gamut(PL_COLOR_PRIM_V_GAMUT));
    REQUIRE(pl_color_primaries_is_wide_gamut(PL_COLOR_PRIM_S_GAMUT));

    struct pl_color_space space = pl_color_space_unknown;
    pl_color_space_merge(&space, &pl_color_space_bt709);
    REQUIRE(pl_color_space_equal(&space, &pl_color_space_bt709));

    // Infer some color spaces
    struct pl_color_space hlg = {
        .primaries = PL_COLOR_PRIM_BT_2020,
        .transfer = PL_COLOR_TRC_HLG,
    };

    pl_color_space_infer(&hlg);
    REQUIRE_CMP(hlg.hdr.max_luma, ==, PL_COLOR_HLG_PEAK, "f");

    struct pl_color_space unknown = {0};
    struct pl_color_space display = {
        .primaries = PL_COLOR_PRIM_BT_709,
        .transfer = PL_COLOR_TRC_BT_1886,
    };

    pl_color_space_infer(&unknown);
    pl_color_space_infer(&display);
    REQUIRE(pl_color_space_equal(&unknown, &display));

    float x, y;
    pl_chroma_location_offset(PL_CHROMA_LEFT, &x, &y);
    REQUIRE_CMP(x, ==, -0.5f, "f");
    REQUIRE_CMP(y, ==,  0.0f, "f");
    pl_chroma_location_offset(PL_CHROMA_TOP_LEFT, &x, &y);
    REQUIRE_CMP(x, ==, -0.5f, "f");
    REQUIRE_CMP(y, ==, -0.5f, "f");
    pl_chroma_location_offset(PL_CHROMA_CENTER, &x, &y);
    REQUIRE_CMP(x, ==,  0.0f, "f");
    REQUIRE_CMP(y, ==,  0.0f, "f");
    pl_chroma_location_offset(PL_CHROMA_BOTTOM_CENTER, &x, &y);
    REQUIRE_CMP(x, ==,  0.0f, "f");
    REQUIRE_CMP(y, ==,  0.5f, "f");

    REQUIRE_CMP(pl_raw_primaries_get(PL_COLOR_PRIM_UNKNOWN), ==,
                pl_raw_primaries_get(PL_COLOR_PRIM_BT_709), "p");

    // Color blindness tests
    float red[3]   = { 1.0, 0.0, 0.0 };
    float green[3] = { 0.0, 1.0, 0.0 };
    float blue[3]  = { 0.0, 0.0, 1.0 };

#define TEST_CONE(model, color)                                                 \
    do {                                                                        \
        float tmp[3] = { (color)[0], (color)[1], (color)[2] };                  \
        pl_matrix3x3 mat = pl_get_cone_matrix(&(model), bt709);                 \
        pl_matrix3x3_apply(&mat, tmp);                                          \
        printf("%s + %s = %f %f %f\n", #model, #color, tmp[0], tmp[1], tmp[2]); \
        for (int i = 0; i < 3; i++)                                             \
            REQUIRE_FEQ((color)[i], tmp[i], 1e-5f);                             \
    } while(0)

    struct pl_cone_params red_only = { .cones = PL_CONE_MS };
    struct pl_cone_params green_only = { .cones = PL_CONE_LS };
    struct pl_cone_params blue_only = pl_vision_monochromacy;

    // These models should all round-trip white
    TEST_CONE(pl_vision_normal, white);
    TEST_CONE(pl_vision_protanopia, white);
    TEST_CONE(pl_vision_protanomaly, white);
    TEST_CONE(pl_vision_deuteranomaly, white);
    TEST_CONE(pl_vision_tritanomaly, white);
    TEST_CONE(pl_vision_achromatopsia, white);
    TEST_CONE(red_only, white);
    TEST_CONE(green_only, white);
    TEST_CONE(blue_only, white);

    // These models should round-trip blue
    TEST_CONE(pl_vision_normal, blue);
    TEST_CONE(pl_vision_protanomaly, blue);
    TEST_CONE(pl_vision_deuteranomaly, blue);

    // These models should round-trip red
    TEST_CONE(pl_vision_normal, red);
    TEST_CONE(pl_vision_tritanomaly, red);
    TEST_CONE(pl_vision_tritanopia, red);

    // These models should round-trip green
    TEST_CONE(pl_vision_normal, green);

    // Color adaptation tests
    struct pl_cie_xy d65 = pl_white_from_temp(6504);
    REQUIRE_FEQ(d65.x, 0.31271, 1e-3);
    REQUIRE_FEQ(d65.y, 0.32902, 1e-3);
    struct pl_cie_xy d55 = pl_white_from_temp(5503);
    REQUIRE_FEQ(d55.x, 0.33242, 1e-3);
    REQUIRE_FEQ(d55.y, 0.34743, 1e-3);

    // Make sure we infer the correct set of metadata parameters
#define TEST_METADATA(CSP, TYPE, MIN, MAX, AVG)                             \
    do {                                                                    \
        float _min, _max, _avg;                                             \
        pl_color_space_nominal_luma_ex(pl_nominal_luma_params(              \
            .color    = &(CSP),                                             \
            .metadata = TYPE,                                               \
            .scaling  = PL_HDR_PQ,                                          \
            .out_min  = &_min,                                              \
            .out_max  = &_max,                                              \
            .out_avg  = &_avg,                                              \
        ));                                                                 \
        const float _min_ref = pl_hdr_rescale(PL_HDR_NITS, PL_HDR_PQ, MIN); \
        const float _max_ref = pl_hdr_rescale(PL_HDR_NITS, PL_HDR_PQ, MAX); \
        const float _avg_ref = pl_hdr_rescale(PL_HDR_NITS, PL_HDR_PQ, AVG); \
        REQUIRE_FEQ(_min, _min_ref, 1e-5);                                  \
        REQUIRE_FEQ(_max, _max_ref, 1e-5);                                  \
        REQUIRE_FEQ(_avg, _avg_ref, 1e-5);                                  \
    } while (0)

    const struct pl_color_space hdr10plus = {
        .primaries = PL_COLOR_PRIM_BT_2020,
        .transfer  = PL_COLOR_TRC_PQ,
        .hdr = {
            .min_luma  = 0.005,
            .max_luma  = 4000,
            .scene_max = {596.69, 1200, 500},
            .scene_avg = 300,
        },
    };

    REQUIRE(pl_hdr_metadata_contains(&hdr10plus.hdr, PL_HDR_METADATA_ANY));
    REQUIRE(pl_hdr_metadata_contains(&hdr10plus.hdr, PL_HDR_METADATA_NONE));
    REQUIRE(pl_hdr_metadata_contains(&hdr10plus.hdr, PL_HDR_METADATA_HDR10));
    REQUIRE(pl_hdr_metadata_contains(&hdr10plus.hdr, PL_HDR_METADATA_HDR10PLUS));
    REQUIRE(!pl_hdr_metadata_contains(&hdr10plus.hdr, PL_HDR_METADATA_CIE_Y));

    TEST_METADATA(hdr10plus, PL_HDR_METADATA_NONE,      PL_COLOR_HDR_BLACK, 10000, 0);
    TEST_METADATA(hdr10plus, PL_HDR_METADATA_CIE_Y,     PL_COLOR_HDR_BLACK, 4000, 0);
    TEST_METADATA(hdr10plus, PL_HDR_METADATA_HDR10,     PL_COLOR_HDR_BLACK, 4000, 0);
    TEST_METADATA(hdr10plus, PL_HDR_METADATA_HDR10PLUS, PL_COLOR_HDR_BLACK, 1000, 250);
    TEST_METADATA(hdr10plus, PL_HDR_METADATA_ANY,       PL_COLOR_HDR_BLACK, 1000, 250);

    const struct pl_color_space dovi = {
        .primaries = PL_COLOR_PRIM_BT_2020,
        .transfer  = PL_COLOR_TRC_PQ,
        .hdr = {
            .min_luma = 0.005,
            .max_luma = 4000,
            .max_pq_y = pl_hdr_rescale(PL_HDR_NITS, PL_HDR_PQ, 1000),
            .avg_pq_y = pl_hdr_rescale(PL_HDR_NITS, PL_HDR_PQ, 250),
        },
    };

    REQUIRE(pl_hdr_metadata_contains(&dovi.hdr, PL_HDR_METADATA_ANY));
    REQUIRE(pl_hdr_metadata_contains(&dovi.hdr, PL_HDR_METADATA_NONE));
    REQUIRE(pl_hdr_metadata_contains(&dovi.hdr, PL_HDR_METADATA_HDR10));
    REQUIRE(pl_hdr_metadata_contains(&dovi.hdr, PL_HDR_METADATA_CIE_Y));
    REQUIRE(!pl_hdr_metadata_contains(&dovi.hdr, PL_HDR_METADATA_HDR10PLUS));

    TEST_METADATA(dovi, PL_HDR_METADATA_NONE,      PL_COLOR_HDR_BLACK, 10000, 0);
    TEST_METADATA(dovi, PL_HDR_METADATA_HDR10,     PL_COLOR_HDR_BLACK, 4000, 0);
    TEST_METADATA(dovi, PL_HDR_METADATA_HDR10PLUS, PL_COLOR_HDR_BLACK, 4000, 0);
    TEST_METADATA(dovi, PL_HDR_METADATA_CIE_Y,     PL_COLOR_HDR_BLACK, 1000, 250);
    TEST_METADATA(dovi, PL_HDR_METADATA_ANY,       PL_COLOR_HDR_BLACK, 1000, 250);

    const struct pl_color_space hlg4000 = {
        .primaries    = PL_COLOR_PRIM_BT_2020,
        .transfer     = PL_COLOR_TRC_HLG,
        .hdr.max_luma = 4000,
        .hdr.min_luma = 0.005,
    };

    TEST_METADATA(hlg4000, PL_HDR_METADATA_NONE,  PL_COLOR_HDR_BLACK, PL_COLOR_HLG_PEAK, 0);
    TEST_METADATA(hlg4000, PL_HDR_METADATA_HDR10, 0.005, 4000, 0);
    TEST_METADATA(hlg4000, PL_HDR_METADATA_ANY,   0.005, 4000, 0);

    const struct pl_color_space untagged = {
        .primaries = PL_COLOR_PRIM_BT_709,
        .transfer  = PL_COLOR_TRC_BT_1886,
    };

    REQUIRE(pl_hdr_metadata_contains(&untagged.hdr, PL_HDR_METADATA_NONE));
    REQUIRE(!pl_hdr_metadata_contains(&untagged.hdr, PL_HDR_METADATA_ANY));
    REQUIRE(!pl_hdr_metadata_contains(&untagged.hdr, PL_HDR_METADATA_HDR10));
    REQUIRE(!pl_hdr_metadata_contains(&untagged.hdr, PL_HDR_METADATA_CIE_Y));
    REQUIRE(!pl_hdr_metadata_contains(&untagged.hdr, PL_HDR_METADATA_HDR10PLUS));

    const float sdr_black = PL_COLOR_SDR_WHITE / PL_COLOR_SDR_CONTRAST;
    TEST_METADATA(untagged, PL_HDR_METADATA_NONE, sdr_black, PL_COLOR_SDR_WHITE, 0);
    TEST_METADATA(untagged, PL_HDR_METADATA_ANY,  sdr_black, PL_COLOR_SDR_WHITE, 0);

    const struct pl_color_space sdr50 = {
        .primaries    = PL_COLOR_PRIM_BT_709,
        .transfer     = PL_COLOR_TRC_BT_1886,
        .hdr.max_luma = 50,
    };

    REQUIRE(pl_hdr_metadata_contains(&sdr50.hdr, PL_HDR_METADATA_NONE));
    REQUIRE(pl_hdr_metadata_contains(&sdr50.hdr, PL_HDR_METADATA_ANY));
    REQUIRE(pl_hdr_metadata_contains(&sdr50.hdr, PL_HDR_METADATA_HDR10));
    REQUIRE(!pl_hdr_metadata_contains(&sdr50.hdr, PL_HDR_METADATA_CIE_Y));
    REQUIRE(!pl_hdr_metadata_contains(&sdr50.hdr, PL_HDR_METADATA_HDR10PLUS));

    TEST_METADATA(sdr50, PL_HDR_METADATA_NONE,  sdr_black, PL_COLOR_SDR_WHITE, 0);
    TEST_METADATA(sdr50, PL_HDR_METADATA_HDR10, 50 / PL_COLOR_SDR_CONTRAST, 50, 0);
    TEST_METADATA(sdr50, PL_HDR_METADATA_ANY,   50 / PL_COLOR_SDR_CONTRAST, 50, 0);

    const struct pl_color_space sdr10k = {
        .primaries    = PL_COLOR_PRIM_BT_709,
        .transfer     = PL_COLOR_TRC_BT_1886,
        .hdr.min_luma = PL_COLOR_SDR_WHITE / 10000,
    };

    REQUIRE(pl_hdr_metadata_contains(&sdr10k.hdr, PL_HDR_METADATA_NONE));
    REQUIRE(!pl_hdr_metadata_contains(&sdr10k.hdr, PL_HDR_METADATA_ANY));
    REQUIRE(!pl_hdr_metadata_contains(&sdr10k.hdr, PL_HDR_METADATA_HDR10));
    TEST_METADATA(sdr10k, PL_HDR_METADATA_NONE,  sdr_black, PL_COLOR_SDR_WHITE, 0);
    TEST_METADATA(sdr10k, PL_HDR_METADATA_HDR10, PL_COLOR_SDR_WHITE / 10000, PL_COLOR_SDR_WHITE, 0);
    TEST_METADATA(sdr10k, PL_HDR_METADATA_ANY,   PL_COLOR_SDR_WHITE / 10000, PL_COLOR_SDR_WHITE, 0);

    const struct pl_color_space bogus_vals = {
        .primaries = PL_COLOR_PRIM_BT_2020,
        .transfer  = PL_COLOR_TRC_HLG,
        .hdr.min_luma = 1e-9,
        .hdr.max_luma = 1000000,
    };

    const struct pl_color_space bogus_flip = {
        .primaries = PL_COLOR_PRIM_BT_2020,
        .transfer  = PL_COLOR_TRC_PQ,
        .hdr.min_luma = 4000,
        .hdr.max_luma = 0.05,
    };

    const struct pl_color_space bogus_sign = {
        .primaries = PL_COLOR_PRIM_BT_2020,
        .transfer  = PL_COLOR_TRC_HLG,
        .hdr.min_luma = -0.5,
        .hdr.max_luma = -4000,
    };

    TEST_METADATA(bogus_vals, PL_HDR_METADATA_HDR10, PL_COLOR_HDR_BLACK, 10000, 0);
    TEST_METADATA(bogus_flip, PL_HDR_METADATA_HDR10, PL_COLOR_HDR_BLACK, 10000, 0);
    TEST_METADATA(bogus_sign, PL_HDR_METADATA_HDR10, PL_COLOR_HDR_BLACK, PL_COLOR_HLG_PEAK, 0);
}
