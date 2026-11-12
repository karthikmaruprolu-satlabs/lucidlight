#pragma once

#include <QQuickTextDocument>
#include <QSyntaxHighlighter>

// JavaScript highlighting for the script editor: keywords, DL. calls,
// strings, comments, and numbers, all in theme-following colors picked at
// highlight time from the Omarchy palette.
class JsHighlighter : public QSyntaxHighlighter {
    Q_OBJECT
    Q_PROPERTY(QQuickTextDocument *document READ document WRITE setDocument)
    Q_PROPERTY(QColor keywordColor READ keywordColor WRITE setKeywordColor NOTIFY colorsChanged)
    Q_PROPERTY(QColor dlColor READ dlColor WRITE setDlColor NOTIFY colorsChanged)
    Q_PROPERTY(QColor stringColor READ stringColor WRITE setStringColor NOTIFY colorsChanged)
    Q_PROPERTY(QColor commentColor READ commentColor WRITE setCommentColor NOTIFY colorsChanged)
    Q_PROPERTY(QColor numberColor READ numberColor WRITE setNumberColor NOTIFY colorsChanged)

public:
    explicit JsHighlighter(QObject *parent = nullptr);

    QQuickTextDocument *document() const { return m_document; }
    void setDocument(QQuickTextDocument *document);

    QColor keywordColor() const { return m_keyword; }
    void setKeywordColor(const QColor &color) { m_keyword = color; rehighlight(); emit colorsChanged(); }
    QColor dlColor() const { return m_dl; }
    void setDlColor(const QColor &color) { m_dl = color; rehighlight(); emit colorsChanged(); }
    QColor stringColor() const { return m_string; }
    void setStringColor(const QColor &color) { m_string = color; rehighlight(); emit colorsChanged(); }
    QColor commentColor() const { return m_comment; }
    void setCommentColor(const QColor &color) { m_comment = color; rehighlight(); emit colorsChanged(); }
    QColor numberColor() const { return m_number; }
    void setNumberColor(const QColor &color) { m_number = color; rehighlight(); emit colorsChanged(); }

signals:
    void colorsChanged();

protected:
    void highlightBlock(const QString &text) override;

private:
    QQuickTextDocument *m_document = nullptr;
    QColor m_keyword;
    QColor m_dl;
    QColor m_string;
    QColor m_comment;
    QColor m_number;
};
