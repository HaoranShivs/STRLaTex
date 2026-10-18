// WP-02：MathCommandCatalog 是面板、补全与宏包诊断的唯一来源。
#include "TestMain.hpp"

#include <set>
#include <string>

#include "math/MathCommandCatalog.h"

using namespace pf;

PF_TEST(MathCatalogCommandsAreUniqueAndWellFormed) {
    const auto& catalog = MathCommandCatalog::Instance();
    PF_CHECK(!catalog.All().empty());
    std::set<std::string> commands;
    for (const auto& command : catalog.All()) {
        PF_CHECK(!command.command.empty());
        PF_CHECK(command.command.front() != '\\');
        PF_CHECK(!command.label.empty());
        PF_CHECK(!command.description.empty());
        PF_CHECK(commands.insert(command.command).second); // 唯一
        PF_CHECK(!command.snippet.empty());
    }
    // 结构/字体按钮必须提供占位符；符号类命令可以只是命令本身。
    for (const auto& command : catalog.InCategory(MathCategory::Structure))
        PF_CHECK(command.snippet.find('$') != std::string::npos);
    for (const auto& command : catalog.InCategory(MathCategory::Font))
        PF_CHECK(command.snippet.find('$') != std::string::npos);
    PF_CHECK(catalog.Find("frac") != nullptr);
    PF_CHECK(catalog.Find("not-a-command") == nullptr);
}

PF_TEST(MathCatalogFindsCategoriesAndSearches) {
    const auto& catalog = MathCommandCatalog::Instance();
    const auto fonts = catalog.InCategory(MathCategory::Font);
    bool has_mathbb = false;
    for (const auto& command : fonts)
        if (command.command == "mathbb")
            has_mathbb = true;
    PF_CHECK(has_mathbb);
    PF_CHECK(catalog.InCategory(MathCategory::Greek).size() >= 20);
    PF_CHECK(catalog.InCategory(MathCategory::Relation).size() >= 8);

    const auto search = catalog.Search("mathb");
    PF_CHECK(!search.empty());
    bool found = false;
    for (const auto& command : search)
        if (command.command == "mathbb")
            found = true;
    PF_CHECK(found);
    PF_CHECK(catalog.Search(" Definitely no such command ").empty());
}

PF_TEST(MathCatalogMapsCommandsToRequiredPackages) {
    const auto& catalog = MathCommandCatalog::Instance();
    auto packages = catalog.RequiredPackages("\\mathbb{R}");
    PF_CHECK(packages.size() == 1 && packages.front() == "amssymb");
    packages = catalog.RequiredPackages("\\mathscr{F}");
    PF_CHECK(packages.size() == 1 && packages.front() == "mathrsfs");
    packages = catalog.RequiredPackages("\\frac{a}{b} + \\alpha");
    PF_CHECK(packages.empty());
    packages = catalog.RequiredPackages("\\mathbb{R} \\substack{x\\\\y} \\frac{a}{b}");
    PF_CHECK(packages == std::vector<std::string>({"amsmath", "amssymb"}));
}
