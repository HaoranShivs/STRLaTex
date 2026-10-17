// Citation token 编解码的单元测试（WP-01 §4.8）。
// 这一层是 Narrative 丢失（P-9）的根因所在：token payload 必须同时保存
// keys 与 mode，旧的 "key1,key2" 载荷仍要按 Parenthetical 解析。
#include "TestMain.hpp"

#include "document/InlineTokenCodec.h"

using namespace pf;

PF_TEST(CitationTokenCodecEncodesModeAndKeys) {
    Citation parenthetical;
    parenthetical.keys = {"smith2024"};
    PF_CHECK(EncodeCitationToken(parenthetical) == "p|smith2024");

    Citation narrative;
    narrative.keys = {"smith2024", "jones2020"};
    narrative.mode = CitationMode::Narrative;
    PF_CHECK(EncodeCitationToken(narrative) == "n|smith2024,jones2020");
}

PF_TEST(CitationTokenCodecDecodesExplicitModes) {
    auto parenthetical = DecodeCitationToken("p|smith2024");
    PF_CHECK(parenthetical.has_value());
    if (parenthetical) {
        PF_CHECK(parenthetical->mode == CitationMode::Parenthetical);
        PF_CHECK(parenthetical->keys == std::vector<std::string>{"smith2024"});
    }

    auto narrative = DecodeCitationToken("n|smith2024,jones2020");
    PF_CHECK(narrative.has_value());
    if (narrative) {
        PF_CHECK(narrative->mode == CitationMode::Narrative);
        PF_CHECK(narrative->keys == (std::vector<std::string>{"smith2024", "jones2020"}));
    }
}

PF_TEST(CitationTokenCodecKeepsLegacyPayloadsParenthetical) {
    // 旧 token（升级前写入的 payload）绝不能因为缺少前缀而丢失。
    auto legacy = DecodeCitationToken("smith2024,jones2020");
    PF_CHECK(legacy.has_value());
    if (legacy) {
        PF_CHECK(legacy->mode == CitationMode::Parenthetical);
        PF_CHECK(legacy->keys == (std::vector<std::string>{"smith2024", "jones2020"}));
    }
}

PF_TEST(CitationTokenCodecTrimsDropsEmptyAndDedupes) {
    auto decoded = DecodeCitationToken(" smith2024 , , jones2020,smith2024 ");
    PF_CHECK(decoded.has_value());
    if (decoded) {
        PF_CHECK(decoded->keys == (std::vector<std::string>{"smith2024", "jones2020"}));
    }

    Citation duplicates;
    duplicates.keys = {"a", " ", "a", "b"};
    PF_CHECK(EncodeCitationToken(duplicates) == "p|a,b");

    Citation empty;
    PF_CHECK(EncodeCitationToken(empty).empty());
    PF_CHECK(!DecodeCitationToken("").has_value());
    PF_CHECK(!DecodeCitationToken("p|").has_value());
    PF_CHECK(!DecodeCitationToken("  ,  ").has_value());
}

PF_TEST(CitationTokenCodecTreatsUnknownPrefixAsLegacyKeyText) {
    // BibTeX key 可以包含 '|'；只有 p/n 前缀是保留的。未知前缀整体
    // 作为旧格式 key 保留，绝不丢弃。
    auto decoded = DecodeCitationToken("x|weird-key");
    PF_CHECK(decoded.has_value());
    if (decoded) {
        PF_CHECK(decoded->mode == CitationMode::Parenthetical);
        PF_CHECK(decoded->keys == std::vector<std::string>{"x|weird-key"});
    }
}
