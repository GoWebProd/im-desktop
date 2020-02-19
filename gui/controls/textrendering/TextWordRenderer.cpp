#include "stdafx.h"

#include "TextWordRenderer.h"
#include "TextRenderingUtils.h"

#include "utils/utils.h"

#include "private/qtextengine_p.h"
#include "private/qfontengine_p.h"
#include "qtextformat.h"

namespace
{
    const int getExtraSpace() { return Utils::scale_value(2); }
    const int getYDiff() { return Utils::scale_value(1); }

    double getVerticalShift(const int _lineHeight, const Ui::TextRendering::VerPosition _pos, const QTextItemInt& _gf)
    {
        switch (_pos)
        {
        case Ui::TextRendering::VerPosition::TOP:
            return _gf.ascent.toReal() + (_lineHeight - (_gf.ascent.toReal() + _gf.descent.toReal())) / (2 / Utils::scale_bitmap_ratio());

        case Ui::TextRendering::VerPosition::BASELINE:
            break;

        case Ui::TextRendering::VerPosition::BOTTOM:
            return -_gf.descent.toReal();

        case Ui::TextRendering::VerPosition::MIDDLE:
            return _gf.ascent.toInt() - (_gf.ascent.toInt() + _gf.descent.toInt()) / 2;
        }

        return 0;
    }

    std::pair<QTextItemInt, QFont> prepareGlyph(const QStackTextEngine& _engine, const int _item)
    {
        const QScriptItem &si = _engine.layoutData->items.at(_item);

        QFont f = _engine.font(si);

        QTextItemInt gf(si, &f);
        gf.glyphs = _engine.shapedGlyphs(&si);
        gf.chars = _engine.layoutData->string.unicode() + si.position;
        gf.num_chars = _engine.length(_item);
        if (_engine.forceJustification)
        {
            for (int j = 0; j < gf.glyphs.numGlyphs; ++j)
                gf.width += gf.glyphs.effectiveAdvance(j);
        }
        else
        {
            gf.width = si.width;
        }
        gf.logClusters = _engine.logClusters(&si);

        return { std::move(gf), std::move(f) }; // gf stores pointer to f inside
    }
}

using namespace TextWordRendererPrivate;

namespace Ui
{
    namespace TextRendering
    {
        struct TextWordRenderer_p
        {
            std::unique_ptr<QStackTextEngine> engine_;
            std::unique_ptr<QTextEngine::LayoutData> releaseMem_;
        };

        TextWordRenderer::TextWordRenderer(
            QPainter* _painter,
            const QPointF& _point,
            const int _lineHeight,
            const int _lineSpacing,
            const int _selectionDiff,
            const VerPosition _pos,
            const QColor& _selectColor,
            const QColor& _highlightColor,
            const QColor& _hightlightTextColor,
            const QColor& _linkColor
            )
            : painter_(_painter)
            , point_(_point)
            , lineHeight_(_lineHeight)
            , lineSpacing_(_lineSpacing)
            , selectionDiff_(_selectionDiff)
            , pos_(_pos)
            , selectColor_(_selectColor)
            , highlightColor_(_highlightColor)
            , hightlightTextColor_(_hightlightTextColor)
            , linkColor_(_linkColor)
            , addSpace_(0)
            , d(std::make_unique<TextWordRenderer_p>())
        {
            assert(painter_);
        }

        TextWordRenderer::~TextWordRenderer() = default;

        void TextWordRenderer::draw(const TextWord& _word, const bool _needsSpace)
        {
            addSpace_ = _word.isSpaceAfter() ? _word.spaceWidth() : 0;

            if (_word.isEmoji())
                drawEmoji(_word, _needsSpace);
            else
                drawWord(_word, _needsSpace);
        }

        void TextWordRenderer::drawWord(const TextWord& _word, const bool _needsSpace)
        {
            const auto text = _word.getText();
            const auto useLinkPen = _word.isLink() && _word.getShowLinks() == Ui::TextRendering::LinksVisible::SHOW_LINKS;
            const auto textColor = useLinkPen ? linkColor_ : _word.getColor();

            textParts_.clear();
            textParts_.push_back(TextPart(&text, textColor));

            if (_word.isHighlighted() && highlightColor_.isValid())
            {
                const auto highlightedTextColor = hightlightTextColor_.isValid() ? hightlightTextColor_ : textColor;
                if (highlightedTextColor == textColor)
                    fill(text, _word.highlightedFrom(), _word.highlightedTo(), highlightColor_);
                else
                    split(text, _word.highlightedFrom(), _word.highlightedTo(), highlightedTextColor, highlightColor_);
            }

            if (_word.isSelected() && selectColor_.isValid())
            {
                const auto selectedTextColor = _word.getColor();
                if (selectedTextColor == textColor && textParts_.size() == 1)
                    fill(text, _word.selectedFrom(), _word.selectedTo(), selectColor_);
                else
                    split(text, _word.selectedFrom(), _word.selectedTo(), selectedTextColor, selectColor_);
            }

            if (textParts_.size() > 1)
                textParts_.erase(std::remove_if(textParts_.begin(), textParts_.end(), [](const auto& p) { return p.ref_.isEmpty(); }), textParts_.end());

            if (!_needsSpace && _word.isSpaceAfter())
            {
                static const QString sp(QChar::Space);
                textParts_.push_back(TextPart(&sp, textColor));

                if (_word.isSpaceSelected())
                    textParts_.back().fill_.push_back({ QStringRef(), selectColor_ });
                else if (_word.isSpaceHighlighted())
                    textParts_.back().fill_.push_back({ QStringRef(), highlightColor_ });
            }

            const auto fillH = lineHeight_ + getYDiff();
            auto fillY = point_.y() - selectionDiff_ + getYDiff();
            if (pos_ == Ui::TextRendering::VerPosition::MIDDLE) //todo: handle other cases
                fillY -= lineHeight_ / 2.0;
            else if (pos_ == Ui::TextRendering::VerPosition::BASELINE)
                fillY -= textAscent(_word.getFont());

            for (const auto& part : textParts_)
            {
                const auto curText = textParts_.size() > 1 ? part.ref_.toString() : text;

                const auto [visualOrder, nItems] = prepareEngine(curText, _word.getFont());

                bool fillHanded = false;
                for (int i = 0; i < nItems; ++i)
                {
                    const auto [gf, font] = prepareGlyph(*d->engine_, visualOrder[i]);

                    const auto x = point_.x();
                    const auto y = point_.y() + getVerticalShift(lineHeight_, pos_, gf) - lineSpacing_ / 2;

                    if (!part.fill_.isEmpty())
                    {
                        for (const auto& f : part.fill_)
                        {
                            if (f.ref_.isEmpty())
                            {
                                painter_->fillRect(QRect(x, fillY, gf.width.ceil().toInt(), fillH), f.color_);
                            }
                            else if (!fillHanded)
                            {
                                const auto leftWidth = textWidth(_word.getFont(), text.left(f.ref_.position()));
                                const auto selWidth = textWidth(_word.getFont(), text.mid(f.ref_.position(), f.ref_.size()));
                                const QRect r(roundToInt(x + leftWidth), fillY, ceilToInt(selWidth), fillH);
                                painter_->fillRect(r, f.color_);
                                fillHanded = true;
                            }
                        }
                    }

                    painter_->setPen(part.textColor_);
                    painter_->drawTextItem(QPointF(x, y), gf);

                    point_.rx() += gf.width.toReal();
                }
            }

            if (_word.isSpaceSelected() || _word.isSpaceHighlighted())
            {
                const auto addedWidth = addSpace_ + getExtraSpace();
                const auto fillColor = _word.isSpaceSelected() ? selectColor_ : highlightColor_;
                if (fillColor.isValid())
                    painter_->fillRect(QRect(point_.x(), fillY, addedWidth, fillH), fillColor);
            }

            if (_needsSpace && _word.isSpaceAfter())
                point_.rx() += addSpace_;
        }

        void TextWordRenderer::drawEmoji(const TextWord& _word, const bool _needsSpace)
        {
            auto emoji = Emoji::GetEmoji(_word.getCode(), _word.emojiSize() * Utils::scale_bitmap_ratio());
            Utils::check_pixel_ratio(emoji);

            const auto b = Utils::scale_bitmap_ratio();
            auto y = point_.y() + (lineHeight_ / 2.0 - emoji.height() / 2.0 / b);

            if (pos_ == VerPosition::MIDDLE) //todo: handle other cases
                y -= lineHeight_ / 2.0;
            else if (pos_ == VerPosition::BASELINE)
                y -= textAscent(_word.getFont());
            else if (pos_ == VerPosition::BOTTOM)
                y -= lineHeight_;

            const bool selected = _word.isFullSelected() && selectColor_.isValid();
            const bool highlighted = _word.isFullHighlighted() && highlightColor_.isValid();
            if (selected || highlighted)
            {
                const QRectF rect(
                    point_.x() - getExtraSpace() / 2,
                    point_.y() - selectionDiff_ + getYDiff(),
                    _word.cachedWidth() + getExtraSpace() / 2,
                    lineHeight_ + getYDiff());

                if (highlighted)
                    painter_->fillRect(rect.adjusted(0, 0, _word.isSpaceHighlighted() ? addSpace_ : 0, 0), highlightColor_);
                if (selected)
                    painter_->fillRect(rect.adjusted(0, 0, _word.isSpaceSelected() ? addSpace_ : 0, 0), selectColor_);
            }

            if (_word.underline())
            {
                auto someSpaces = qsl(" ");
                const auto fontMetrics = getMetrics(_word.getFont());
                while (fontMetrics.width(someSpaces) <= emoji.width() / b)
                    someSpaces += ql1c(' ');
                if (!_needsSpace && _word.isSpaceAfter())
                    someSpaces += ql1c(' ');

                const auto [visualOrder, nItems] = prepareEngine(someSpaces, _word.getFont());

                auto uy = y;
                for (int i = 0; i < nItems; ++i)
                {
                    const auto [gf, f] = prepareGlyph(*d->engine_, visualOrder[i]);

                    uy += getVerticalShift(lineHeight_, pos_, gf);
                    uy -= 1.*lineSpacing_ / 2;
                    uy = platform::is_apple() ? std::ceil(uy) : std::floor(uy);
                    painter_->setPen((selected || highlighted) ? _word.getColor() : linkColor_);
                    painter_->drawTextItem(QPointF(point_.x() - 1., uy), gf);
                }
            }

            const auto imageRect = QRectF(point_.x(), y, emoji.width() / b, emoji.height() / b);
            painter_->drawImage(imageRect, emoji);

            point_.rx() += (roundToInt(_word.cachedWidth()) + addSpace_);
        }

        void TextWordRenderer::split(const QString& _text, const int _from, const int _to, const QColor& _textColor, const QColor& _fillColor)
        {
            const auto processFillRightSide = [](const auto _it, const auto& _oldFill)
            {
                for (auto f : _oldFill)
                {
                    if (f.ref_.isEmpty())
                    {
                        _it->fill_.push_back(std::move(f));
                    }
                    else if (f.ref_.position() + f.ref_.size() >= _it->from())
                    {
                        f.ref_ = f.ref_.mid(_it->from());
                        _it->fill_.push_back(std::move(f));
                    }
                }
            };

            const int prevSize = textParts_.size() - 1;
            for (int i = prevSize; i >= 0; --i)
            {
                auto& p = textParts_[i];

                if (_to <= p.from() || _from >= p.to()) //not included
                    continue;

                const auto r = p.to();
                if (_from <= p.from() && _to >= p.to()) //full included
                {
                    p.textColor_ = _textColor;
                    p.fillEntirely(_fillColor);
                }
                else if (_from > p.from() && _to < p.to()) //inside
                {
                    auto partFill = p.fill_;
                    p.truncate(_from - p.from());
                    textParts_.insert(std::next(textParts_.begin(), i + 1), TextPart(_text.midRef(_from, _to - _from), _textColor, _fillColor));
                    auto it = textParts_.insert(std::next(textParts_.begin(), i + 2), TextPart(_text.midRef(_to, r - _to), p.textColor_));
                    processFillRightSide(it, partFill);
                }
                else if (_from <= p.from() && _to < p.to()) //left
                {
                    auto partFill = p.fill_;
                    p.truncate(_to - p.from());

                    auto it = textParts_.insert(std::next(textParts_.begin(), i + 1), TextPart(_text.midRef(_to, r - _to), p.textColor_));
                    processFillRightSide(it, partFill);

                    p.textColor_ = _textColor;
                    p.fillEntirely(_fillColor);
                }
                else //right
                {
                    p.truncate(_from - p.from());
                    textParts_.insert(std::next(textParts_.begin(), i + 1), TextPart(_text.midRef(_from,  r - _from), _textColor, _fillColor));
                }
            }
        }

        void TextWordRenderer::fill(const QString& _text, const int _from, const int _to, const QColor& _fillColor)
        {
            for (auto& p : textParts_)
            {
                if (_to <= p.from() || _from >= p.to()) //not included
                    continue;

                if (_from <= p.from() && _to >= p.to()) //full included
                {
                    p.fillEntirely(_fillColor);
                }
                else //partially included
                {
                    const int from = std::clamp(_from, p.from(), p.to());
                    const int to = std::clamp(_to, p.from(), p.to());
                    if (from != to)
                        p.fill_.push_back({ _text.midRef(from, to - from), _fillColor });
                }
            }
        }

        std::pair<QVarLengthArray<int>, int> TextWordRenderer::prepareEngine(const QString& _text, const QFont& _font)
        {
            auto& engine = d->engine_;
            if (!engine || engine->font() != _font)
            {
                engine = std::make_unique<QStackTextEngine>(QString(), _font);
                engine->option.setTextDirection(Qt::LeftToRight);
                engine->fnt = _font;
            }

            engine->text = _text;
            engine->layoutData = nullptr;
            engine->itemize();

            // in engine.itemize() memory for engine.layoutData is allocated (ALOT!)
            d->releaseMem_ = std::unique_ptr<QTextEngine::LayoutData>(engine->layoutData);

            QScriptLine line;
            line.length = _text.size();
            engine->shapeLine(line);

            const int nItems = engine->layoutData->items.size();
            QVarLengthArray<int> visualOrder(nItems);
            QVarLengthArray<uchar> levels(nItems);
            for (int i = 0; i < nItems; ++i)
                levels[i] = engine->layoutData->items[i].analysis.bidiLevel;
            QTextEngine::bidiReorder(nItems, levels.data(), visualOrder.data());
            return { std::move(visualOrder), nItems };
        }
    }
}