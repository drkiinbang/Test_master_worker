#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""
JSON에서 workers 목록을 읽어:
1) 개인키 무비번 접속 여부 확인
2) 불가 시 GUI(별표 마스킹)로 비밀번호 입력 → 공개키 배포
3) 배포 후 개인키 접속 재검증
4) 종료 시 성공/건너뜀/실패 요약 출력

사용:
  python deploy_and_test_key.py --config workers_list.json
"""

import argparse, json, os, posixpath, socket, sys, traceback
import paramiko

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

# --------------- 배포/검증 ---------------
def deploy_key_with_password(host, port, username, password, pubkey_path):
    expanded_pub = os.path.expanduser(pubkey_path)
    if not os.path.exists(expanded_pub):
        raise FileNotFoundError(f"공개키 파일을 찾을 수 없습니다: {expanded_pub}")
    with open(expanded_pub, "r", encoding="utf-8") as f:
        public_key_text = f.read().strip()

    ssh = paramiko.SSHClient()
    ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    try:
        try:
            ssh.connect(host, port=port, username=username, password=password,
                        look_for_keys=False, allow_agent=False,
                        timeout=15, banner_timeout=15, auth_timeout=15)
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
                "auth_keys": auth_keys, "changed": changed}
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
    pub = srv.get("pubkey") or cfg.get("default_pubkey_path") or cfg.get("default_key_path")
    if not pub:
        raise ValueError("공개키 경로가 누락되었습니다. 'pubkey' 또는 'default_pubkey_path'를 지정하세요.")
    pub = os.path.expanduser(pub)
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
            print("[INFO ] 개인키 경로가 없거나 파일이 없음 → 배포 시도")

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
