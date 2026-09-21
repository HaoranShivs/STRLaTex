// 富文本行内编辑器测试的驱动入口：这些测试会构建控件，因此需要
// QApplication。该对象在堆上分配，并在 main 返回之前销毁——静态的
// QApplication 若在退出时销毁，会与测试框架的静态对象发生析构顺序冲突，
// 并在所有用例都已通过之后触发段错误。
#include <QApplication>

#include "TestMain.hpp"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    const int result = testfw::RunAll();
    std::cout.flush();
    // 显式销毁 application，使其析构过程不会在退出时与测试框架的静态对象
    // 竞争。
    return result;
}
