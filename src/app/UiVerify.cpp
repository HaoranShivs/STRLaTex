// UI 验证：打开一个真实项目、填充内容，并对主窗口的各个状态
// （欢迎页、工作区、斜杠菜单、问题面板）截图。
#include <QApplication>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTimer>
#include <filesystem>
#include <iostream>

#include "app/InlineEditor.h"
#include "app/MainWindow.h"
#include "app/ProjectController.h"

using namespace pf::gui;

int main(int argc, char *argv[]) {
  QApplication app(argc, argv);
  std::filesystem::remove_all("/tmp/pf-ui-demo");
  int failures = 0;
  auto check = [&](bool ok, const char *what) {
    std::cout << (ok ? "[ OK  ] " : "[FAIL] ") << what << "\n";
    if (!ok)
      ++failures;
  };

  MainWindow window;
  window.show();

  QTimer::singleShot(300, [&]() {
    // 1. 通过 controller 创建并填充一个项目。
    ProjectController *controller = window.controller();
    check(controller != nullptr, "controller reachable");

    // PF_UI_PROJECT 会打开一个已有项目而非构建演示项目，
    // 以便在真实稿件渲染时对其进行检查。
    const QString existing = qEnvironmentVariable("PF_UI_PROJECT");
    QString dir =
        existing.isEmpty() ? QStringLiteral("/tmp/pf-ui-demo") : existing;
    if (!existing.isEmpty()) {
      check(window.OpenProjectDir(dir), "open existing project");
    } else {
      check(controller->NewProject(dir), "new project");
      controller->StartAutosave();
      // 以编程方式把主窗口切换到工作区视图。
      // （通过对话框调用 NewProject 会调用 ShowWorkspace；直接使用 controller
      // 会绕过它，因此要通过窗口 API 打开。）
      controller->session().Save();
      controller->CloseProject();
      bool opened = window.OpenProjectDir(dir);
      if (!opened) {
        // 恢复式打开只会在 project.paper 缺失时失败；直接重试一次
        // 以便把错误暴露出来。
        std::string err;
        opened = controller->session().OpenProject(dir.toStdString(), &err);
        std::cout << "  open error: " << err << "\n";
      }
      check(opened, "open via window (workspace shown)");
    }

    if (!existing.isEmpty()) {
      // 已有项目：先按打开时的状态截图，再展示重排（re-flow）
      // 与悬停插入提示在真实内容上的效果。
      QTimer::singleShot(1800, [&window]() {
        window.grab().save("/tmp/pf-ui-workspace.png");
        std::cout << "[ SAVE ] workspace (as opened)\n";

        auto pump = [](int ms) {
          QElapsedTimer timer;
          timer.start();
          while (timer.elapsed() < ms) {
            QApplication::processEvents(QEventLoop::AllEvents, 20);
          }
        };
        auto find_row = [&window](const QString &key) -> QPlainTextEdit * {
          for (QPlainTextEdit *edit : window.findChildren<QPlainTextEdit *>()) {
            if (edit->isVisible() &&
                edit->property("row_focus_key").toString() == key) {
              return edit;
            }
          }
          return nullptr;
        };

        // 点入摘要再离开会提交该行，
        // 这条路径可以柔化硬换行粘贴的内容。
        if (QPlainTextEdit *abstract = find_row("front:abstract")) {
          abstract->setFocus(Qt::MouseFocusReason);
          pump(150);
          if (QPlainTextEdit *again = find_row("front:abstract")) {
            again->clearFocus();
          }
          pump(600);
          window.grab().save("/tmp/pf-ui-reflowed.png");
          std::cout << "[ SAVE ] abstract after commit (re-flowed)\n";
        }

        // 悬停到最后一个块之后的插入条上。
        std::vector<QWidget *> gaps;
        for (QWidget *w : window.findChildren<QWidget *>()) {
          if (w->property("gap_anchor").isValid() && w->isVisible()) {
            gaps.push_back(w);
          }
        }
        std::cout << "[ INFO ] live gaps: " << gaps.size() << "\n";
        if (!gaps.empty()) {
          QWidget *gap = gaps.back();
          QEvent enter(QEvent::Enter);
          QApplication::sendEvent(gap, &enter);
          pump(400);
          window.grab().save("/tmp/pf-ui-gap.png");
          std::cout << "[ SAVE ] hover insert affordance\n";
        }
        QApplication::exit(0);
      });
      return;
    }

    controller->SetTitle("Weakly Supervised Infrared Small Target Detection");
    controller->SetAffiliationsText(
        "School of Information and Communication Engineering, University "
        "One; Institute of Optoelectronics, Institute Two");
    controller->SetAuthorsText(
        "Tanran Shi\u00b9\u00b2 · Author B\u00b9 · Author C\u00b2");
    controller->SetAbstract(
        "Infrared small target detection has attracted considerable "
        "attention in recent years, yet robust detection under complex "
        "backgrounds remains difficult because targets occupy only a few "
        "pixels and carry almost no texture. This paper studies a weakly "
        "supervised formulation that learns from image-level labels.");
    controller->SetAuthorsText(
        "Tanran Shi\u00b9\u00b2 · Author B\u00b9 · Author C\u00b2");
    controller->SetKeywordsText("infrared, small target, deep learning");
    auto sec = controller->InsertSection("Introduction");
    // 长度足以到达第二页，这样预览就能显示
    // 第 1 张与第 2 张纸之间的接缝。
    for (int i = 0; i < 26; ++i) {
      controller->InsertParagraph(
          sec.created_node,
          QString("Paragraph %1. Infrared small target detection under "
                  "complex backgrounds remains difficult because "
                  "targets occupy only a few pixels, carry almost no "
                  "texture, and are easily confused with cloud edges, "
                  "rooftops and wave crests in the surrounding scene.")
              .arg(i + 1));
    }
    controller->InsertParagraph(
        sec.created_node,
        "Infrared small target detection has received considerable "
        "attention in recent years. Previous methods primarily rely on "
        "hand-crafted features, which degrade quickly when the target "
        "contrast drops or when the background contains cluttered "
        "structures such as cloud edges, rooftops and wave crests. Deep "
        "models trained end to end have improved recall, but they still "
        "need pixel-level annotations that are expensive to obtain and "
        "inconsistent between annotators. We therefore investigate a "
        "weakly supervised pipeline: image-level labels drive a "
        "coarse-to-fine refinement stage, and a segmentation head "
        "recovers the target mask without any per-pixel supervision. "
        "Experiments on public benchmarks show consistent gains over "
        "hand-crafted baselines while reducing annotation cost.");
    controller->InsertEquation(sec.created_node,
                               "L = L_{seg} + \\lambda L_{aux}", true);
    // 带行内数学公式的富文本段落：12pt 正文字体必须让公式
    // 保持在文本基线上并处于行框内（UI 方案 §6）。
    {
      const pf::EditResult rich_para =
          controller->InsertParagraphAfter(sec.created_node, QString());
      if (rich_para.status == pf::EditStatus::Applied &&
          !rich_para.created_node.empty()) {
        pf::InlineContent rich;
        rich.push_back(pf::TextRun{"The weighting coefficient ", 0});
        pf::InlineMath lambda;
        lambda.expression.latex = "\\lambda = 0.7";
        rich.push_back(lambda);
        rich.push_back(pf::TextRun{" balances the auxiliary term, and ", 0});
        pf::InlineMath frac;
        frac.expression.latex = "\\frac{\\partial u}{\\partial t}";
        rich.push_back(frac);
        rich.push_back(pf::TextRun{
            " drives the refinement stage down the image sequence.", 0});
        controller->EditParagraphRich(rich_para.created_node, rich);
      }
    }
    controller->ImportBibliographyText(
        QString("@article{wang2025, author={W. Wang}, title={Infrared Target "
                "Detection}, year={2025}}\n"
                "@article{li2024, author={L. Li}, title={Small Target Survey}, "
                "year={2024}}"));

    // 触发视图刷新（每次编辑都会发出 documentChanged；
    // 给事件循环一点时间）。
    QTimer::singleShot(400, [&window, controller, &failures]() {
      window.grab().save("/tmp/pf-ui-workspace.png");
      std::cout << "[ SAVE ] workspace screenshot\n";

      auto pump = [](int ms) {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < ms) {
          QApplication::processEvents(QEventLoop::AllEvents, 20);
        }
      };

      // 聚焦外观（UI 方案 §2/§9/§10）：聚焦某个 Text 行会显示
      // 强调线、仅悬停时出现的「Text」标签、格式栏，
      // 并在大纲中高亮其所属章节。
      for (InlineEditor *rich : window.findChildren<InlineEditor *>()) {
        if (!rich->isVisible())
          continue;
        rich->setFocus(Qt::MouseFocusReason);
        pump(350);
        window.grab().save("/tmp/pf-ui-focus.png");
        std::cout << "[ SAVE ] focused text row (chrome + outline)\n";
        break;
      }

      // 焦点编辑模式（UI 方案 §11）：侧边面板收起。
      for (QPushButton *button : window.findChildren<QPushButton *>()) {
        if (!button->text().contains("Focus"))
          continue;
        button->click();
        pump(400);
        window.grab().save("/tmp/pf-ui-focusmode.png");
        std::cout << "[ SAVE ] focus editing mode\n";
        button->click();
        pump(400);
        break;
      }

      // 3. 构建；当一次 build 被接受时，controller 会发出带类型的
      // previewUpdated 事件（已在应用线程上）。
      // 初始化阶段的编辑会经由防抖的自动构建逐步排空，并可能
      // 取代第一次请求（最新者胜），因此重试若干次，
      // 直到当前 revision 的 build 成功为止。
      static int build_attempts = 0;
      ProjectController *ctl = controller;
      QObject::connect(controller, &ProjectController::previewUpdated,
                       [&window, ctl](const pf::PreviewUpdate &update) {
                         std::cout << "[ EVNT ] previewUpdated success="
                                   << update.success << "\n";
                         if (update.success) {
                           QMetaObject::invokeMethod(
                               &window,
                               [&window]() {
                                 QTimer::singleShot(1200, [&window]() {
                                   window.grab().save("/tmp/pf-ui-built.png");
                                   std::cout << "[ SAVE ] built "
                                                "screenshot\n";
                                   // 缩放预览并
                                   // 截取结果。
                                   // 把各页显示为
                                   // 连续的纵向排列。
                                   window.ZoomPreviewForTest(0.0, 0.0, 0.45);
                                   QTimer::singleShot(600, [&window]() {
                                     window.grab().save("/tmp/"
                                                        "pf-ui-pages."
                                                        "png");
                                     std::cout << "[ SAVE ] "
                                                  "page seam"
                                                  " screenshot"
                                                  "\n";
                                     window.ZoomPreviewForTest(2.5, 0.05, 0.12);
                                   });
                                   QTimer::singleShot(1500, [&window]() {
                                     window.grab().save("/tmp/"
                                                        "pf-ui-zoomed."
                                                        "png");
                                     std::cout << "[ SAVE ] "
                                                  "zoomed "
                                                  "screenshot"
                                                  "\n";
                                     QApplication::exit(0);
                                   });
                                 });
                               },
                               Qt::QueuedConnection);
                           return;
                         }
                         if (++build_attempts >= 6) {
                           std::cout << "[FAIL] build did not succeed\n";
                           QApplication::exit(1);
                         }
                         QMetaObject::invokeMethod(
                             ctl, [ctl]() { ctl->RequestBuild(true); },
                             Qt::QueuedConnection);
                       });
      controller->RequestBuild(true);
    });
  });

  QTimer::singleShot(60000, [&]() { QApplication::exit(1); });
  return app.exec();
}
