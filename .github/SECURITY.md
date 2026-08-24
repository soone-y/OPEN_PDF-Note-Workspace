# セキュリティポリシー / Security Policy

PDF Note Workspace の安全性向上にご協力いただきありがとうございます。

This document contains the security policy in Japanese and English.

## サポート対象のバージョン

セキュリティ修正を提供するのは、GitHub Releases で公開している**最新版**だけです。過去のリリース、開発中のスナップショット、独自に改変・再配布された版はサポート対象外です。

| バージョン | セキュリティ更新 |
| --- | --- |
| GitHub Releases の最新版 | 対象 |
| 上記以外のすべての版 | 対象外 |

問題を報告する前に、可能であれば最新版で再現するか確認してください。古い版でのみ再現する場合でも、最新版への更新で解消しないことが分かれば報告に含めてください。

## 脆弱性の報告方法

公開 Issue、Discussion、SNS などに、脆弱性の詳細や再現手順を投稿しないでください。GitHub リポジトリの **Security** タブにある **Report a vulnerability** から、非公開で報告してください。

報告には、分かる範囲で次の情報を含めてください。

- 使用したアプリのバージョン、配布物の種類（通常版または Lite版）、Windows のバージョン
- 問題を再現するための具体的な手順と、期待した結果・実際の結果
- 想定される影響（情報の閲覧・改ざん、任意コード実行、サービス不能など）
- 必要であれば、再現に使った最小限のファイルまたは安全な代替手順
- 修正案、概念実証コード、関連するログや画面表示

個人情報、機密情報、実在の PDF・ノート・認証情報は送らないでください。再現データは、内容を置き換えた最小限のものを使用してください。

リポジトリ側で非公開報告を受け付けられない状態の場合は、脆弱性の詳細を公開しないでください。リポジトリ管理者が非公開の報告窓口を有効化するまでお待ちください。

## 対応と公開

報告を受け取った後、最新版への影響を確認し、必要に応じて修正・検証・リリースを行います。対応時期や修正の提供を保証するものではありません。

修正が必要な場合は、利用者が安全に更新できるよう、原則として修正版の公開後に詳細を共有します。報告者が公開を希望する場合も、修正と利用者への案内が整うまで公開を控えてください。

## 対象範囲

このポリシーは、公式 GitHub Releases の PDF Note Workspace 本体および同梱ファイルを対象とします。アプリは外部通信を行わない方針のため、オンラインサービスやアカウント機能は提供していません。

## English

### Supported versions

Security fixes are provided only for the latest version published through GitHub Releases. Older releases, development snapshots, and independently modified or redistributed builds are not supported.

| Version | Security updates |
| --- | --- |
| Latest GitHub Release | Supported |
| All other versions | Not supported |

Before reporting an issue, please check whether it can be reproduced with the latest release when possible. If it occurs only with an older version, include whether updating resolves it.

### Reporting a vulnerability

Do not publish vulnerability details or reproduction steps in public Issues, Discussions, or social media. Use **Report a vulnerability** in the repository's **Security** tab to submit a private report.

Include, where known:

- Application version, edition (Full or Lite), and Windows version
- Exact reproduction steps, expected result, and actual result
- Expected impact, such as disclosure or modification of information, arbitrary code execution, or denial of service
- A minimal safe reproduction file or safe alternative steps, if needed
- A proposed fix, proof of concept, and relevant logs or screen text

Do not send personal information, confidential information, real PDFs or notes, or credentials. Use the smallest possible reproduction data with its contents replaced.

If private reporting is unavailable for the repository, do not disclose vulnerability details publicly. Wait until the repository maintainer enables a private reporting channel.

### Handling and disclosure

After receiving a report, we assess the latest release and, when needed, fix, verify, and release an update. We cannot guarantee a response time or that a fix will be provided.

When a fix is needed, details are normally shared after a fixed release is available so that users can update safely. Please avoid public disclosure until the fix and user guidance are ready, even if you wish to disclose the report.

### Scope

This policy covers the official GitHub Releases of PDF Note Workspace and their bundled files. The application is designed not to make external network connections and does not provide online services or account features.
