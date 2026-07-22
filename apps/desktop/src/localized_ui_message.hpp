#pragma once

#include <QByteArray>
#include <QCoreApplication>
#include <QLocale>
#include <QString>
#include <QVector>

#include <concepts>
#include <initializer_list>
#include <type_traits>
#include <utility>
#include <variant>

class LocalizedUiArgument final {
public:
  struct Text final {
    QByteArray context;
    QByteArray source;

    [[nodiscard]] bool operator==(const Text &) const = default;
  };

  struct Number final {
    double value = 0.0;
    char format = 'g';
    int precision = -1;

    [[nodiscard]] bool operator==(const Number &) const = default;
  };

  LocalizedUiArgument(QString value) : value_(std::move(value)) {}

  template <std::integral Integer>
    requires(!std::same_as<std::remove_cv_t<Integer>, bool>)
  LocalizedUiArgument(const Integer value) {
    if constexpr (std::is_signed_v<Integer>) {
      value_ = static_cast<qint64>(value);
    } else {
      value_ = static_cast<quint64>(value);
    }
  }

  LocalizedUiArgument(const double value) : value_(value) {}
  LocalizedUiArgument(Text value) : value_(std::move(value)) {}
  LocalizedUiArgument(Number value) : value_(value) {}

  [[nodiscard]] static Text translatedText(const char *const context,
                                           const char *const source) {
    return {
        .context = QByteArray(context),
        .source = QByteArray(source),
    };
  }

  [[nodiscard]] static Number formattedNumber(const double value,
                                              const char format,
                                              const int precision) noexcept {
    return {
        .value = value,
        .format = format,
        .precision = precision,
    };
  }

  [[nodiscard]] QString rendered(const bool localized) const {
    return std::visit(
        [localized](const auto &value) -> QString {
          using Value = std::decay_t<decltype(value)>;
          if constexpr (std::same_as<Value, QString>) {
            return value;
          } else if constexpr (std::same_as<Value, qint64>) {
            return localized
                       ? QLocale().toString(static_cast<qlonglong>(value))
                       : QString::number(static_cast<qlonglong>(value));
          } else if constexpr (std::same_as<Value, quint64>) {
            return localized
                       ? QLocale().toString(static_cast<qulonglong>(value))
                       : QString::number(static_cast<qulonglong>(value));
          } else if constexpr (std::same_as<Value, double>) {
            return localized ? QLocale().toString(value, 'g', -1)
                             : QString::number(value, 'g', -1);
          } else if constexpr (std::same_as<Value, Text>) {
            return QCoreApplication::translate(value.context.constData(),
                                               value.source.constData());
          } else {
            return localized
                       ? QLocale().toString(value.value, value.format,
                                            value.precision)
                       : QString::number(value.value, value.format,
                                         value.precision);
          }
        },
        value_);
  }

  [[nodiscard]] bool operator==(const LocalizedUiArgument &) const = default;

private:
  std::variant<QString, qint64, quint64, double, Text, Number> value_;
};

class LocalizedUiMessage final {
public:
  LocalizedUiMessage() = default;

  LocalizedUiMessage(
      const char *const context, const char *const source,
      const std::initializer_list<LocalizedUiArgument> arguments = {})
      : context_(context), source_(source), arguments_(arguments) {}

  [[nodiscard]] bool isEmpty() const noexcept { return source_.isEmpty(); }

  void clear() noexcept {
    context_.clear();
    source_.clear();
    arguments_.clear();
  }

  [[nodiscard]] QString translated() const {
    if (isEmpty()) {
      return {};
    }
    const QString message =
        QCoreApplication::translate(context_.constData(), source_.constData());

    // Expand placeholders from the translated template in one pass. Repeated
    // QString::arg calls would rescan already-inserted diagnostics, so a path or
    // decoder error containing text such as "%1" could be corrupted by a later
    // argument.
    QString result;
    result.reserve(message.size());
    qsizetype cursor = 0;
    while (cursor < message.size()) {
      if (message.at(cursor) != QLatin1Char('%')) {
        result += message.at(cursor++);
        continue;
      }

      const qsizetype placeholder_start = cursor;
      ++cursor;
      bool localized = false;
      if (cursor < message.size() && message.at(cursor) == QLatin1Char('L')) {
        localized = true;
        ++cursor;
      }
      if (cursor >= message.size() || message.at(cursor) < QLatin1Char('1') ||
          message.at(cursor) > QLatin1Char('9')) {
        result +=
            message.mid(placeholder_start, cursor - placeholder_start);
        continue;
      }

      qsizetype argument_number = 0;
      while (cursor < message.size() &&
             message.at(cursor) >= QLatin1Char('0') &&
             message.at(cursor) <= QLatin1Char('9')) {
        argument_number =
            argument_number * 10 +
            (message.at(cursor).unicode() - QLatin1Char('0').unicode());
        ++cursor;
      }
      if (argument_number <= 0 || argument_number > arguments_.size()) {
        result +=
            message.mid(placeholder_start, cursor - placeholder_start);
        continue;
      }
      result += arguments_.at(argument_number - 1).rendered(localized);
    }
    return result;
  }

  [[nodiscard]] bool operator==(const LocalizedUiMessage &) const = default;

private:
  QByteArray context_;
  QByteArray source_;
  QVector<LocalizedUiArgument> arguments_;
};
