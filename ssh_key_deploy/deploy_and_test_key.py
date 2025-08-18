#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""
workers_list.json 에서 workers 목록을 읽어:
1) 개인키 무비번 접속 여부 확인
2) 불가 시 비밀번호 다이얼로그(별표 마스킹)로 입력받아 공개키 배포
   - 공개키(.pub)가 없다면: 자동으로 키쌍 생성 후 배포
3) 배포 후 개인키 접속 재검증
4) 종료 시 성공/건너뜀/실패 요약 출력

사용:
  python deploy_and_test_key.py --config workers_list.json
JSON 예시:
{
  "default_pubkey_path": "~/.ssh/id_ed25519.pub",
  "workers": [
    { "host": "10.10.10.13", "port": 22, "username": "kiinbang" },
    { "host": "10.10.10.24", "port": 22, "username": "kiinbang" }
  ]
}
"""

import argparse, json, os, posixpath, socket, sys, traceback, subprocess, stat
import paramiko

def _normalize_path(p):
    if p is None: return None
    p = str(p).strip().strip('"').strip("'")
    p = os.path.expandvars(p)
    p = os.path.expanduser(p)
    return os.path.normpath(p)

def _known_hosts_path() -> str:
    return _normalize_path("~/.ssh/known_hosts")

def _known_host_label(host: str, port: int) -> str:
    """OpenSSH known_hosts 라벨 규칙: 기본포트(22)는 host, 그 외는 [host]:port"""
    # IPv6, 콜론 포함 등은 대괄호로 감쌉니다.
    needs_bracket = (":" in host)
    if port == 22 and not needs_bracket:
        return host
    return f"[{host}]:{port}"

def _ensure_known_hosts_entry(host: str, port: int, pkey) -> None:
    """
    Paramiko PKey(원격 서버 호스트키)를 OpenSSH known_hosts 라인으로 기록.
    중복이면 건너뜀.
    """
    path = _known_hosts_path()
    ssh_dir = os.path.dirname(path)
    os.makedirs(ssh_dir, exist_ok=True)
    label = _known_host_label(host, port)
    line = f"{label} {pkey.get_name()} {pkey.get_base64()}"
    try:
        with open(path, "r", encoding="utf-8") as f:
            content = f.read()
    except FileNotFoundError:
        content = ""

    if line not in content:
        with open(path, "a", encoding="utf-8") as f:
            f.write(line + "\n")
        # 권한 보정(유닉스 계열)
        if os.name == "posix":
            try: os.chmod(ssh_dir, 0o700)
            except Exception: pass
            try: os.chmod(path, 0o644)
            except Exception: pass
        print(f"[LOCAL] known_hosts 등록: {label} ({pkey.get_name()})")
    else:
        print(f"[LOCAL] known_hosts 이미 등록됨: {label}")

# ---------------- GUI 유틸 ----------------
def ask_password_masked(title: str, prompt: str):
    """가능하면 Tk 다이얼로그(별표 마스킹), 불가하면 콘솔 getpass로 폴백."""
    try:
        import tkinter as tk
        from tkinter import simpledialog
        root = tk.Tk(); root.withdraw()
        try:
            pwd = simpledialog.askstring(title, prompt, show='*')
        finally:
            try: root.destroy()
            except Exception: pass
        return pwd
    except Exception:
        import getpass
        try:
            pwd = getpass.getpass(f"{title} - {prompt}: ")
            return pwd if pwd else None
        except (EOFError, KeyboardInterrupt):
            return None

def show_error_dialog(title: str, message: str):
    try:
        import tkinter as tk
        from tkinter import messagebox
        root = tk.Tk(); root.withdraw()
        try: messagebox.showerror(title, message)
        finally:
            try: root.destroy()
            except Exception: pass
    except Exception:
        print(f"[ERROR] {title}: {message}")

def ask_retry_cancel(title: str, message: str) -> bool:
    try:
        import tkinter as tk
        from tkinter import messagebox
        root = tk.Tk(); root.withdraw()
        try: return messagebox.askretrycancel(title, message)
        finally:
            try: root.destroy()
            except Exception: pass
    except Exception:
        print(f"[WARN ] {title}: {message} (no GUI; not retrying)")
        return False

# --------------- 예외 ---------------
class PasswordAuthError(Exception): ...
class SshConnectError(Exception): ...

# --------------- 키/플랫폼/권한 유틸 ---------------
def _load_private_key(priv_path, passphrase=None):
    exc = None
    for KeyCls in (paramiko.Ed25519Key, paramiko.RSAKey, paramiko.ECDSAKey):
        try:
            return KeyCls.from_private_key_file(priv_path, password=passphrase)
        except Exception as e:
            exc = e
    raise exc

def _detect_remote_platform(ssh):
    try:
        _stdin, _stdout, _stderr = ssh.exec_command(
            'powershell -NoProfile -NonInteractive -Command "$PSVersionTable.PSVersion.Major"', timeout=10)
        if _stdout.channel.recv_exit_status() == 0:
            return "windows"
    except Exception:
        pass
    try:
        _stdin, _stdout, _stderr = ssh.exec_command("uname", timeout=10)
        if _stdout.channel.recv_exit_status() == 0:
            return "posix"
    except Exception:
        pass
    return "windows"

def _ensure_ssh_dir_and_perms(ssh, platform, home_dir):
    ssh_dir = posixpath.join(home_dir, ".ssh")
    auth_keys = posixpath.join(ssh_dir, "authorized_keys")
    if platform == "posix":
        ssh.exec_command(f"mkdir -p '{ssh_dir}' && chmod 700 '{ssh_dir}'")
        ssh.exec_command(f"touch '{auth_keys}' && chmod 600 '{auth_keys}'")
    else:
        ps = rf"""
$ErrorActionPreference='Stop'
$ssh='{ssh_dir}'
$auth='{auth_keys}'
if (!(Test-Path $ssh)) {{ New-Item -ItemType Directory -Path $ssh | Out-Null }}
if (!(Test-Path $auth)) {{ New-Item -ItemType File -Path $auth | Out-Null }}
$u = "$env:USERNAME"
icacls $ssh /inheritance:r | Out-Null
icacls $ssh /grant:r $u:(OI)(CI)(F) /T | Out-Null
icacls $auth /inheritance:r | Out-Null
icacls $auth /grant:r $u:(R,W) | Out-Null
"""
        ssh.exec_command(f"powershell -NoProfile -NonInteractive -Command \"{ps}\"")
    return ssh_dir, auth_keys

def _append_key_if_missing(ssh, sftp, auth_keys, public_key_text):
    existing = ""
    try:
        with sftp.open(auth_keys, "r") as f:
            existing = f.read().decode(errors="replace")
    except IOError:
        existing = ""
    if public_key_text.strip() not in existing:
        with sftp.open(auth_keys, "a") as f:
            f.write(public_key_text.strip() + "\n")
        return True
    return False

# -------- 로컬 키쌍 보장(없으면 생성) --------
def _posix() -> bool:
    return os.name == "posix"

def _ensure_dir_and_perms(path_dir: str):
    os.makedirs(path_dir, exist_ok=True)
    if _posix():
        os.chmod(path_dir, 0o700)

def _fix_key_permissions(priv_path: str, pub_path: str):
    if _posix():
        try: os.chmod(priv_path, 0o600)
        except Exception: pass
        try: os.chmod(pub_path, 0o644)
        except Exception: pass

def _default_comment():
    user = os.environ.get("USER") or os.environ.get("USERNAME") or "user"
    host = socket.gethostname()
    return f"{user}@{host}"

def _ensure_local_keypair(pub_path: str, key_type: str = "ed25519", passphrase: str | None = None, comment: str | None = None):
    """
    pub_path가 가리키는 공개키가 없으면:
      1) priv만 있으면 → ssh-keygen -y 로 pub 생성 (폴백: Paramiko로 생성)
      2) 둘 다 없으면 → ssh-keygen 으로 키쌍 생성 (폴백: Paramiko RSA 3072)
    return: (priv_path, pub_path, created_flag)  # created_flag: "none"|"pub_created"|"pair_created"
    """
    pub_path = os.path.expanduser(pub_path)
    if not pub_path.endswith(".pub"):
        raise ValueError("public key 경로는 .pub 확장자를 포함해야 합니다.")
    priv_path = pub_path[:-4]

    key_dir = os.path.dirname(priv_path) or os.path.expanduser("~/.ssh")
    _ensure_dir_and_perms(key_dir)

    priv_exists = os.path.exists(priv_path)
    pub_exists  = os.path.exists(pub_path)

    if priv_exists and pub_exists:
        _fix_key_permissions(priv_path, pub_path)
        return priv_path, pub_path, "none"

    # pub만 없는 경우 → priv에서 pub 추출
    if priv_exists and not pub_exists:
        # 1) ssh-keygen -y 시도
        try:
            res = subprocess.run(
                ["ssh-keygen", "-y", "-f", priv_path],
                capture_output=True, text=True, check=False
            )
            if res.returncode == 0 and res.stdout.strip():
                with open(pub_path, "w", encoding="utf-8") as f:
                    line = res.stdout.strip()
                    if comment := (comment or _default_comment()):
                        if " " in line:
                            # "type base64 [comment]" 형태일 수 있으니 comment 유무에 따라 보정
                            parts = line.split()
                            if len(parts) >= 2:
                                line = f"{parts[0]} {parts[1]} {comment}"
                    f.write(line + "\n")
                _fix_key_permissions(priv_path, pub_path)
                return priv_path, pub_path, "pub_created"
        except FileNotFoundError:
            pass  # ssh-keygen 없음 → 아래 폴백

        # 2) Paramiko로 공개키 라인 구성 (RSA/ECDSA 가능)
        try:
            key = _load_private_key(priv_path, passphrase=None)
            line = f"{key.get_name()} {key.get_base64()} {(comment or _default_comment())}"
            with open(pub_path, "w", encoding="utf-8") as f:
                f.write(line + "\n")
            _fix_key_permissions(priv_path, pub_path)
            return priv_path, pub_path, "pub_created"
        except Exception as e:
            raise RuntimeError(f"기존 개인키에서 공개키 생성 실패: {e}")

    # 둘 다 없는 경우 → 키쌍 생성
    # 1) ssh-keygen 시도 (ed25519 기본)
    try:
        args = ["ssh-keygen", "-t", key_type, "-f", priv_path, "-N", passphrase or "", "-C", comment or _default_comment()]
        res = subprocess.run(args, capture_output=True, text=True, check=False)
        if res.returncode == 0 and os.path.exists(priv_path) and os.path.exists(pub_path):
            _fix_key_permissions(priv_path, pub_path)
            return priv_path, pub_path, "pair_created"
    except FileNotFoundError:
        pass  # ssh-keygen 없음

    # 2) 폴백: Paramiko RSA 3072 생성
    key = paramiko.RSAKey.generate(3072)
    key.write_private_key_file(priv_path)  # passphrase 미설정
    line = f"{key.get_name()} {key.get_base64()} {(comment or _default_comment())}"
    with open(pub_path, "w", encoding="utf-8") as f:
        f.write(line + "\n")
    _fix_key_permissions(priv_path, pub_path)
    return priv_path, pub_path, "pair_created"

# --------------- 배포/검증 ---------------
def deploy_key_with_password(host, port, username, password, pubkey_path):
    # 공개키가 없으면 여기서 키쌍까지 만들어 둔다.
    priv_path, pub_path, created = _ensure_local_keypair(pubkey_path)
    if created != "none":
        print(f"[LOCAL] 키 생성: {created} (priv: {priv_path}, pub: {pub_path})")

    with open(pub_path, "r", encoding="utf-8") as f:
        public_key_text = f.read().strip()

    ssh = paramiko.SSHClient()
    ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    try:
        try:
            ssh.connect(host, port=port, username=username, password=password,
                        look_for_keys=False, allow_agent=False,
                        timeout=15, banner_timeout=15, auth_timeout=15)
            
            server_key = ssh.get_transport().get_remote_server_key()
            _ensure_known_hosts_entry(host, port, server_key)

        except paramiko.AuthenticationException as e:
            raise PasswordAuthError("비밀번호가 올바르지 않습니다.") from e
        except (paramiko.SSHException, socket.error) as e:
            raise SshConnectError(f"SSH 연결 실패: {e}") from e

        # 홈 디렉터리 확인
        stdin, stdout, stderr = ssh.exec_command("echo $HOME")
        home_dir = stdout.read().decode().strip()
        if not home_dir:
            stdin, stdout, stderr = ssh.exec_command(
                "powershell -NoProfile -NonInteractive -Command \"$env:USERPROFILE\"")
            home_dir = stdout.read().decode().strip()
        if not home_dir:
            raise RuntimeError("원격 홈 디렉터리를 확인할 수 없습니다.")

        platform = _detect_remote_platform(ssh)
        ssh_dir, auth_keys = _ensure_ssh_dir_and_perms(ssh, platform, home_dir)

        sftp = ssh.open_sftp()
        try:
            changed = _append_key_if_missing(ssh, sftp, auth_keys, public_key_text)
        finally:
            sftp.close()

        if platform == "posix":
            ssh.exec_command(f"chmod 700 '{ssh_dir}' && chmod 600 '{auth_keys}'")

        return {"platform": platform, "home": home_dir, "ssh_dir": ssh_dir,
                "auth_keys": auth_keys, "changed": changed, "priv_path": priv_path}
    finally:
        try: ssh.close()
        except Exception: pass

def test_key_login(host, port, username, privkey_path, passphrase=None):
    privkey_path = os.path.expanduser(privkey_path)
    if not os.path.exists(privkey_path):
        raise FileNotFoundError(f"개인키 파일을 찾을 수 없습니다: {privkey_path}")
    pkey = _load_private_key(privkey_path, passphrase)

    ssh = paramiko.SSHClient()
    ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    try:
        ssh.connect(host, port=port, username=username, pkey=pkey,
                    look_for_keys=False, allow_agent=False,
                    timeout=15, banner_timeout=15, auth_timeout=15)
        
        server_key = ssh.get_transport().get_remote_server_key()
        _ensure_known_hosts_entry(host, port, server_key)

        platform = _detect_remote_platform(ssh)
        cmd = "echo OK:$(whoami) on $(hostname)" if platform == "posix" else \
              ("powershell -NoProfile -NonInteractive -Command "
               "\"Write-Output ('OK:'+ $env:USERNAME +' on '+ $env:COMPUTERNAME)\"")
        _stdin, _stdout, _stderr = ssh.exec_command(cmd)
        exit_code = _stdout.channel.recv_exit_status()
        out = _stdout.read().decode(errors="replace").strip()
        err = _stderr.read().decode(errors="replace").strip()
        return {"exit": exit_code, "stdout": out, "stderr": err, "platform": platform}
    finally:
        try: ssh.close()
        except Exception: pass

# --------------- 설정 로드/경로 결정 ---------------
def _load_config(path):
    with open(path, "r", encoding="utf-8-sig") as f:
        return json.load(f)

def _resolve_pub_priv_paths(cfg, srv):
    # pub 우선순위: 서버별(pubkey) → 전역(default_pubkey_path) → (하위호환) default_key_path
    pub = srv.get("pubkey") or cfg.get("default_pubkey_path") or cfg.get("default_key_path")
    if not pub:
        # 최후의 기본값: ~/.ssh/id_ed25519.pub
        pub = "~/.ssh/id_ed25519.pub"
    pub = os.path.expanduser(pub)

    # priv는 pub에서 자동 유도(.pub 제거) — JSON에 별도 지정할 필요 없음
    priv = srv.get("privkey") or cfg.get("default_privkey_path")
    if not priv and pub.endswith(".pub"):
        priv = pub[:-4]
    priv = os.path.expanduser(priv) if priv else None
    return pub, priv

# --------------- 메인 ---------------
def main():
    ap = argparse.ArgumentParser(description="Deploy SSH public keys using JSON; prompt password via GUI when needed.")
    ap.add_argument("--config", default="workers_list.json", help="JSON 설정 파일 경로 (기본: workers_list.json)")
    args = ap.parse_args()

    try:
        cfg = _load_config(args.config)
    except Exception as e:
        print(f"[FATAL] JSON 설정 로드 실패: {e}")
        sys.exit(2)

    nodes = cfg.get("workers", None)
    if nodes is None:
        nodes = cfg.get("servers", [])  # 하위호환
    if not isinstance(nodes, list) or not nodes:
        print("[FATAL] 'workers' 배열이 비어있거나 잘못되었습니다. (구버전 'servers'도 없음)")
        sys.exit(2)

    successes, failures, skipped = [], [], []

    for i, srv in enumerate(nodes, 1):
        host = srv.get("host")
        port = int(srv.get("port", 22))
        username = srv.get("username")
        if not all([host, username]):
            print(f"[{i:02d}] 구성 누락 → host/username 필요")
            failures.append((f"{host}:{port}", "구성 누락"))
            continue

        try:
            pubkey_path, privkey_path = _resolve_pub_priv_paths(cfg, srv)
        except Exception as e:
            print(f"[{i:02d}] {host}:{port} 구성 오류 → {e}")
            failures.append((f"{host}:{port}", f"구성 오류: {e}"))
            continue

        print(f"\n[CHECK {i:02d}] 키 인증 접속 확인 → {username}@{host}:{port}")
        key_ok = False
        # 개인키가 존재하면 먼저 무비번 접속 확인
        if privkey_path and os.path.exists(privkey_path):
            try:
                r = test_key_login(host, port, username, privkey_path)
                if r["exit"] == 0 and r["stdout"].startswith("OK:"):
                    print("[OK   ] 이미 키 인증 접속이 가능합니다. 배포 불필요.")
                    successes.append((f"{host}:{port}", "이미 배포됨(무비번 OK)"))
                    key_ok = True
                else:
                    print("[WARN ] 접속됨이나 기대 출력과 다름 → 배포 시도")
            except Exception as e:
                print(f"[INFO ] 현재는 키 인증 접속 불가 → 배포 시도 ({e})")
        else:
            print("[INFO ] 개인키가 없거나 경로 미지정 → 배포 시도(필요 시 자동 생성)")

        if key_ok:
            continue

        # -------- 비밀번호 입력 → 배포 루프 --------
        while True:
            password = srv.get("password")  # (하위호환) JSON에 있으면 사용, 보안상 권장 X
            if not password:
                password = ask_password_masked("SSH Password Required",
                                               f"Enter password for {username}@{host}:{port}")
            if not password:
                print(f"[SKIP ] 사용자 취소 → {host}:{port} 배포 건너뜀")
                skipped.append((f"{host}:{port}", "사용자 취소"))
                break

            print(f"[STEP 1] 공개키 배포 시도 → {username}@{host}:{port}")
            try:
                info = deploy_key_with_password(host, port, username, password, pubkey_path)
                print(f"[INFO ] 플랫폼: {info['platform']} / 홈: {info['home']}")
                print(f"[INFO ] SSH 디렉터리: {info['ssh_dir']}")
                print(f"[INFO ] authorized_keys: {info['auth_keys']}")
                print(f"[INFO ] 키 {'추가됨' if info['changed'] else '이미 존재'}")
                # 배포 단계에서 필요시 키쌍을 생성했으므로 priv 경로 보정
                if not privkey_path:
                    privkey_path = info.get("priv_path")
            except PasswordAuthError:
                show_error_dialog("Authentication Failed", "비밀번호가 올바르지 않습니다. 다시 시도하세요.")
                continue
            except SshConnectError as e:
                if ask_retry_cancel("SSH 연결 실패", f"{e}\n재시도하시겠습니까?"):
                    continue
                failures.append((f"{host}:{port}", f"연결 실패: {e}"))
                break
            except FileNotFoundError as e:
                show_error_dialog("키 파일 없음", str(e))
                failures.append((f"{host}:{port}", f"키 파일 없음: {e}"))
                break
            except Exception as e:
                msg = f"예상치 못한 오류: {e}\n{traceback.format_exc(limit=1)}"
                if ask_retry_cancel("배포 오류", msg + "\n재시도하시겠습니까?"):
                    continue
                failures.append((f"{host}:{port}", f"배포 오류: {e}"))
                break

            # 배포 성공 → 키 인증 재테스트
            if not privkey_path or not os.path.exists(privkey_path):
                note = "배포됨(개인키 파일 부재로 테스트 불가)"
                print(f"[WARN ] {note}")
                successes.append((f"{host}:{port}", note))
                break

            print(f"[STEP 2] 키 인증 재접속 테스트 → {privkey_path}")
            try:
                r = test_key_login(host, port, username, privkey_path)
                if r["exit"] == 0 and r["stdout"].startswith("OK:"):
                    print("[OK   ] 키 인증 접속 성공 (암호 프롬프트 없이 접속됨).")
                    successes.append((f"{host}:{port}", "배포됨(무비번 OK)"))
                else:
                    print("[WARN ] 접속은 되었으나 기대 출력과 다름")
                    successes.append((f"{host}:{port}", "배포됨(검증 경고)"))
            except Exception as e:
                print(f"[ERROR] 키 인증 테스트 실패 → {e}")
                failures.append((f"{host}:{port}", f"검증 실패: {e}"))
            break  # 루프 종료

    # ---------- 요약 ----------
    print("\n========== 배포 요약 ==========")
    if successes:
        print(f"[성공] {len(successes)}대");  [print(f"  - {h} : {note}") for h, note in successes]
    else:
        print("[성공] 0대")

    if skipped:
        print(f"[건너뜀] {len(skipped)}대"); [print(f"  - {h} : {note}") for h, note in skipped]
    else:
        print("[건너뜀] 0대")

    if failures:
        print(f"[실패] {len(failures)}대"); [print(f"  - {h} : {reason}") for h, reason in failures]
    else:
        print("[실패] 0대")
    print("================================")

    sys.exit(1 if failures else 0)

if __name__ == "__main__":
    main()
