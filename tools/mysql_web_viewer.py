#!/usr/bin/env python3
"""
Minimal localhost-only MySQL table viewer.

It reads credentials from server-side files/.env and connects through the SSH
tunnel created by tools/mysql_tunnel.py.
"""

import argparse
import html
import os
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlencode, urlparse


REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_ENV_FILE = REPO_ROOT / "server-side files" / ".env"


def load_env_file(path):
    values = {}
    if not path.exists():
        return values

    with path.open("r", encoding="utf-8") as file_obj:
        for raw_line in file_obj:
            line = raw_line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, value = line.split("=", 1)
            values[key.strip()] = value.strip().strip('"').strip("'")
    return values


def import_pymysql():
    extra_path = os.environ.get("MYSQL_VIEWER_PYTHONPATH", "")
    if extra_path and extra_path not in sys.path:
        sys.path.insert(0, extra_path)
    try:
        import pymysql
        return pymysql
    except ModuleNotFoundError:
        print("Missing dependency: pymysql", file=sys.stderr)
        print("Install for this session:", file=sys.stderr)
        print("  python3 -m pip install --target /tmp/esp32_mysql_viewer_deps pymysql", file=sys.stderr)
        print("Then run:", file=sys.stderr)
        print("  MYSQL_VIEWER_PYTHONPATH=/tmp/esp32_mysql_viewer_deps python3 tools/mysql_web_viewer.py", file=sys.stderr)
        raise


def esc(value):
    if value is None:
        return "<span class='null'>NULL</span>"
    return html.escape(str(value))


def quote_ident(name):
    return "`" + str(name).replace("`", "``") + "`"


class ViewerState:
    def __init__(self, args, env_values, pymysql):
        self.args = args
        self.env_values = env_values
        self.pymysql = pymysql

    @property
    def database(self):
        return self.args.database or self.env_values.get("MYSQL_DB", "").strip()

    def connect(self):
        return self.pymysql.connect(
            host=self.args.mysql_host,
            port=self.args.mysql_port,
            user=(self.args.mysql_user or self.env_values.get("MYSQL_USER", "")).strip(),
            password=self.args.mysql_password or self.env_values.get("MYSQL_PASS", ""),
            database=self.database or None,
            charset="utf8mb4",
            cursorclass=self.pymysql.cursors.DictCursor,
            connect_timeout=5,
            read_timeout=10,
            write_timeout=10,
        )


def render_page(title, body):
    return f"""<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>{html.escape(title)}</title>
  <style>
    :root {{
      color-scheme: light;
      --bg: #f5f7fb;
      --panel: #ffffff;
      --text: #172033;
      --muted: #667085;
      --border: #d8dee9;
      --accent: #0f766e;
      --accent-soft: #e6fffb;
      --danger: #b42318;
    }}
    * {{ box-sizing: border-box; }}
    body {{
      margin: 0;
      font: 14px/1.45 -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
      background: var(--bg);
      color: var(--text);
    }}
    header {{
      height: 52px;
      display: flex;
      align-items: center;
      gap: 16px;
      padding: 0 20px;
      background: var(--panel);
      border-bottom: 1px solid var(--border);
      position: sticky;
      top: 0;
      z-index: 10;
    }}
    header a {{ color: var(--accent); text-decoration: none; font-weight: 600; }}
    main {{ padding: 18px 20px; }}
    .layout {{ display: grid; grid-template-columns: 260px minmax(0, 1fr); gap: 18px; }}
    .panel {{
      background: var(--panel);
      border: 1px solid var(--border);
      border-radius: 8px;
      overflow: hidden;
    }}
    .panel h2 {{
      font-size: 14px;
      margin: 0;
      padding: 12px 14px;
      border-bottom: 1px solid var(--border);
      background: #fbfcfe;
    }}
    .table-list a {{
      display: flex;
      justify-content: space-between;
      gap: 8px;
      padding: 9px 14px;
      border-bottom: 1px solid #eef1f5;
      color: var(--text);
      text-decoration: none;
    }}
    .table-list a.active {{ background: var(--accent-soft); color: #0b5f59; font-weight: 700; }}
    .muted {{ color: var(--muted); }}
    .toolbar {{
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: 12px;
      padding: 12px 14px;
      border-bottom: 1px solid var(--border);
    }}
    .toolbar form {{ display: flex; gap: 8px; align-items: center; }}
    input, select, button {{
      height: 32px;
      border: 1px solid var(--border);
      border-radius: 6px;
      padding: 0 9px;
      background: #fff;
      color: var(--text);
    }}
    button {{
      background: var(--accent);
      border-color: var(--accent);
      color: #fff;
      font-weight: 700;
      cursor: pointer;
    }}
    .table-wrap {{ overflow: auto; max-height: calc(100vh - 155px); }}
    table {{ border-collapse: collapse; width: 100%; min-width: 720px; }}
    th, td {{
      border-bottom: 1px solid #eef1f5;
      border-right: 1px solid #eef1f5;
      padding: 7px 9px;
      text-align: left;
      vertical-align: top;
      white-space: nowrap;
      max-width: 360px;
      overflow: hidden;
      text-overflow: ellipsis;
    }}
    th {{
      position: sticky;
      top: 0;
      background: #fbfcfe;
      z-index: 2;
      font-size: 12px;
      color: var(--muted);
    }}
    .error {{ color: var(--danger); padding: 14px; }}
    .null {{ color: #98a2b3; font-style: italic; }}
  </style>
</head>
<body>
  <header>
    <strong>MySQL Viewer</strong>
    <span class="muted">localhost only</span>
    <a href="/">刷新</a>
  </header>
  <main>{body}</main>
</body>
</html>"""


def query_tables(conn):
    with conn.cursor() as cur:
        cur.execute("SHOW TABLES")
        key = next(iter(cur.description))[0]
        return [row[key] for row in cur.fetchall()]


def query_table(conn, table, limit, offset):
    with conn.cursor() as cur:
        cur.execute(f"SELECT COUNT(*) AS cnt FROM {quote_ident(table)}")
        total = int(cur.fetchone()["cnt"])
        cur.execute(f"SELECT * FROM {quote_ident(table)} LIMIT %s OFFSET %s", (limit, offset))
        rows = cur.fetchall()
        columns = list(rows[0].keys()) if rows else []
        if not columns:
            cur.execute(f"SHOW COLUMNS FROM {quote_ident(table)}")
            columns = [row["Field"] for row in cur.fetchall()]
        return total, columns, rows


class ViewerHandler(BaseHTTPRequestHandler):
    state = None

    def log_message(self, fmt, *args):
        return

    def send_html(self, html_text, status=200):
        data = html_text.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        parsed = urlparse(self.path)
        params = parse_qs(parsed.query)
        selected_table = params.get("table", [""])[0]
        limit = min(max(int(params.get("limit", ["100"])[0] or 100), 1), 500)
        offset = max(int(params.get("offset", ["0"])[0] or 0), 0)

        try:
            with self.state.connect() as conn:
                tables = query_tables(conn)
                if not selected_table and tables:
                    selected_table = tables[0]

                table_links = []
                for table in tables:
                    active = " active" if table == selected_table else ""
                    href = "/?" + urlencode({"table": table, "limit": limit})
                    table_links.append(f"<a class='{active}' href='{href}'><span>{esc(table)}</span></a>")

                left = "<div class='panel table-list'><h2>Tables</h2>" + "".join(table_links) + "</div>"

                if selected_table:
                    total, columns, rows = query_table(conn, selected_table, limit, offset)
                    header = "".join(f"<th>{esc(col)}</th>" for col in columns)
                    body_rows = []
                    for row in rows:
                        body_rows.append("<tr>" + "".join(f"<td>{esc(row.get(col))}</td>" for col in columns) + "</tr>")

                    prev_offset = max(offset - limit, 0)
                    next_offset = offset + limit
                    prev_href = "/?" + urlencode({"table": selected_table, "limit": limit, "offset": prev_offset})
                    next_href = "/?" + urlencode({"table": selected_table, "limit": limit, "offset": next_offset})
                    pager = f"<a href='{prev_href}'>上一页</a> <a href='{next_href}'>下一页</a>"
                    if next_offset >= total:
                        pager = f"<a href='{prev_href}'>上一页</a> <span class='muted'>下一页</span>"

                    right = f"""
                    <div class="panel">
                      <div class="toolbar">
                        <div><strong>{esc(selected_table)}</strong> <span class="muted">{offset + 1 if total else 0}-{min(offset + limit, total)} / {total}</span></div>
                        <form method="get">
                          <input type="hidden" name="table" value="{html.escape(selected_table)}">
                          <label class="muted">limit</label>
                          <input name="limit" type="number" min="1" max="500" value="{limit}">
                          <button type="submit">应用</button>
                        </form>
                        <div>{pager}</div>
                      </div>
                      <div class="table-wrap">
                        <table><thead><tr>{header}</tr></thead><tbody>{''.join(body_rows)}</tbody></table>
                      </div>
                    </div>
                    """
                else:
                    right = "<div class='panel'><div class='error'>没有找到表。</div></div>"

                self.send_html(render_page("MySQL Viewer", f"<div class='layout'>{left}{right}</div>"))
        except Exception as exc:
            self.send_html(render_page("MySQL Viewer Error", f"<div class='panel error'>{esc(exc)}</div>"), status=500)


def parse_args():
    parser = argparse.ArgumentParser(description="Localhost-only MySQL table viewer.")
    parser.add_argument("--env-file", default=str(DEFAULT_ENV_FILE))
    parser.add_argument("--mysql-host", default="127.0.0.1")
    parser.add_argument("--mysql-port", type=int, default=3307)
    parser.add_argument("--mysql-user", default="")
    parser.add_argument("--mysql-password", default="")
    parser.add_argument("--database", default="")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8765)
    return parser.parse_args()


def main():
    args = parse_args()
    pymysql = import_pymysql()
    env_values = load_env_file(Path(args.env_file).expanduser())

    ViewerHandler.state = ViewerState(args, env_values, pymysql)
    server = ThreadingHTTPServer((args.host, args.port), ViewerHandler)
    print(f"MySQL viewer: http://{args.host}:{args.port}/")
    print("Press Ctrl+C to stop.")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    raise SystemExit(main())
