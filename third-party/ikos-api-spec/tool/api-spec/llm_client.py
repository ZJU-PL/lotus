###############################################################################
#
# IKOS API spec -- LLM chat client
#
# Minimal OpenAI-compatible chat completions client built on the standard
# library only (urllib), so the generator runs under any Python, including
# ikos' bundled interpreter that ships no third-party packages.
#
# Notices:
#
# Copyright (c) 2018-2023 United States Government as represented by the
# Administrator of the National Aeronautics and Space Administration.
# All Rights Reserved.
#
# Disclaimers:
#
# No Warranty: THE SUBJECT SOFTWARE IS PROVIDED "AS IS" WITHOUT WARRANTY OF
# ANY KIND, EITHER EXPRESSED, IMPLIED, OR STATUTORY, INCLUDING, BUT NOT
# LIMITED TO, ANY WARRANTY THAT THE SUBJECT SOFTWARE WILL CONFORM TO
# SPECIFICATIONS, ANY IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
# PARTICULAR PURPOSE, OR FREEDOM FROM INFRINGEMENT, ANY WARRANTY THAT, IF
# PROVIDED, THE DOCUMENTATION WILL CONFORM TO THE SUBJECT SOFTWARE.  NO
# GOVERNMENT AGENCY, NOR ANY EMPLOYEE THEREOF, ASSUMES ANY LEGAL LIABILITY OR
# RESPONSIBILITY FOR THE ACCURACY, COMPLETENESS, OR USEFULNESS OF ANY
# INFORMATION, APPARATUS, PRODUCT, OR PROCESS DISCLOSED, OR REPRESENTS THAT
# ITS USE WOULD NOT INFRINGE PRIVATELY OWNED RIGHTS.
#
###############################################################################
"""OpenAI-compatible chat completions client (standard library only).

Speaks the ``POST {base-url}/chat/completions`` dialect shared by OpenAI,
DeepSeek, GLM, Qwen, vLLM servers and Ollama's compatibility endpoint.
:data:`PROVIDERS` holds ready-to-use presets for OpenAI, DeepSeek and GLM
(base URL, API key environment variable, default model); any other
OpenAI-compatible endpoint just needs ``--base-url`` and ``--model``.  The
only surface the generator needs is :meth:`ChatClient.complete`, returning
the assistant message text plus the token ``usage`` report.

Transport failures worth retrying (HTTP 429/5xx, connection and timeout
errors) are retried with exponential backoff inside the client; everything
else raises :class:`LLMError` with a message assembled for the user, so the
caller can treat one bad function as one failed generation round instead of
a crash.
"""
import json
import os
import random
import socket
import time
import urllib.error
import urllib.request
from collections import namedtuple

DEFAULT_BASE_URL = 'https://api.openai.com/v1'

# HTTP status codes worth another attempt after a pause
RETRYABLE_HTTP = frozenset((429, 500, 502, 503, 504))

# transport exceptions that mean "endpoint unreachable right now"
RETRYABLE_EXC = (urllib.error.URLError, socket.timeout, ConnectionError, OSError)


class LLMError(Exception):
    """A fatal error talking to the LLM endpoint; str(e) is user-facing."""


class FnBudgetExhausted(LLMError):
    """The per-function wall-clock budget (--fn-budget) ran out.

    Subclasses LLMError so pre-existing LLMError catch sites stay
    graceful, but carries the is_fn_budget marker so other layers can
    re-raise it past their broad `except Exception` harness-failure
    handlers (difftest duck-types the marker, no import needed): a
    budget cut must land as 'failed', never as a skipped gate that
    installs an unvalidated stub.
    """

    is_fn_budget = True


# Built-in provider presets: choosing one with --provider sets the base URL,
# the API key environment variable(s) and a default model; --model,
# --base-url and --api-key-env still override any of those.  Model names
# move fast -- they are only defaults and examples here, never requirements.
Provider = namedtuple('Provider',
                      'name base_url key_envs default_model models'
                      ' temperature extra_body',
                      defaults=(None,))

PROVIDERS = {
    'openai': Provider(
        name='openai',
        base_url='https://api.openai.com/v1',
        key_envs=('OPENAI_API_KEY',),
        default_model='gpt-5.4',
        models=('gpt-5.4', 'gpt-5.4-mini', 'gpt-6-sol', 'gpt-6-luna'),
        # GPT-5.x reasoning models reject the temperature parameter; None
        # leaves it out of the request entirely
        temperature=None,
    ),
    'deepseek': Provider(
        name='deepseek',
        base_url='https://api.deepseek.com/v1',
        key_envs=('DEEPSEEK_API_KEY',),
        default_model='deepseek-flash',
        models=('deepseek-flash', 'deepseek-v4-pro', 'deepseek-v4-flash'),
        temperature=0.2,
        # DeepSeek accepts the Anthropic-style thinking switch in its
        # OpenAI-compatible endpoint.  Without it both V4 arms emit hidden
        # reasoning_content even when the final answer is short.
        extra_body={'thinking': {'type': 'disabled'}},
    ),
    'deepseek-thinking': Provider(
        name='deepseek',
        base_url='https://api.deepseek.com/v1',
        key_envs=('DEEPSEEK_API_KEY',),
        default_model='deepseek-flash',
        models=('deepseek-flash', 'deepseek-v4-pro'),
        temperature=0.2,
        extra_body={'thinking': {'type': 'enabled'}},
    ),
    'glm': Provider(
        name='glm',
        base_url='https://open.bigmodel.cn/api/paas/v4',
        key_envs=('GLM_API_KEY', 'ZHIPUAI_API_KEY', 'ZAI_API_KEY'),
        default_model='glm-5.3',
        models=('glm-5.3', 'glm-5.3-flash', 'glm-5.2'),
        # GLM-5.3 always thinks and its examples pin temperature 1.0
        temperature=1.0,
    ),
    'qwen': Provider(
        name='qwen',
        base_url='https://dashscope.aliyuncs.com/compatible-mode/v1',
        key_envs=('DASHSCOPE_API_KEY',),
        default_model='qwen3.8-flash',
        models=('qwen3.8-max-0902', 'qwen3.8-flash'),
        # same 0.2 the bare-endpoint mode (--base-url) always used, so
        # switching the qwen arms to the preset changes nothing else
        temperature=0.2,
        # The generator uses non-streaming responses.  Qwen's default
        # thinking mode can leave such a response silent for many minutes;
        # disable it so a timed generation arm does not spend its whole
        # budget waiting for hidden reasoning.
        extra_body={'enable_thinking': False},
    ),
    'qwen-thinking': Provider(
        name='qwen',
        base_url='https://dashscope.aliyuncs.com/compatible-mode/v1',
        key_envs=('DASHSCOPE_API_KEY',),
        default_model='qwen3.8-flash',
        models=('qwen3.8-max-0902', 'qwen3.8-flash'),
        temperature=0.2,
        extra_body={'enable_thinking': True},
    ),
}


def find_api_key(provider):
    """The API key of a provider: the first set environment variable.

    Keys are never stored in this file or the container image; the user
    supplies them at runtime through the provider's environment
    variable(s) (or --api-key-env, which outranks them).

    -> (key, where it came from), or (None, None) when none is set.
    """
    for name in provider.key_envs:
        key = os.environ.get(name)
        if key:
            return key, name
    return None, None


class ChatClient:
    """One conversation-round client for an OpenAI-compatible endpoint."""

    def __init__(self,
                 model,
                 base_url=DEFAULT_BASE_URL,
                 api_key=None,
                 timeout=120,
                 temperature=0.2,
                 reasoning_effort=None,
                 max_attempts=4,
                 extra_body=None,
                 deadline=None):
        # temperature None means "send no temperature parameter at all"
        # (reasoning models that reject the option); reasoning_effort None
        # likewise sends nothing (the endpoint default, e.g. GLM's highest
        # thinking effort -- minutes of silent server-side thinking per
        # call on hard prompts)
        self.model = model
        self.base_url = base_url.rstrip('/')
        self.api_key = api_key
        self.timeout = timeout
        self.temperature = temperature
        self.reasoning_effort = reasoning_effort
        self.max_attempts = max_attempts
        self.extra_body = dict(extra_body) if extra_body else None
        self.deadline = deadline

    def complete(self, messages):
        """Run one chat round; -> (content, usage-dict).

        ``messages`` is the full conversation so far (``role``/``content``
        dicts, starting with the system prompt), so repair rounds can show
        the model its previous attempt.  Raises LLMError after exhausting
        retries or on any unrecoverable failure (bad URL, non-retryable
        HTTP status, malformed response).
        """
        body = {'model': self.model,
                'messages': messages}
        if self.temperature is not None:
            body['temperature'] = self.temperature
        if self.reasoning_effort is not None:
            body['reasoning_effort'] = self.reasoning_effort
        if self.extra_body:
            body.update(self.extra_body)
        data = json.dumps(body).encode('utf-8')
        url = '%s/chat/completions' % self.base_url
        headers = {'Content-Type': 'application/json'}
        if self.api_key:
            headers['Authorization'] = 'Bearer %s' % self.api_key
        request = urllib.request.Request(url,
                                         data=data,
                                         headers=headers,
                                         method='POST')
        last_error = 'unknown error'
        for attempt in range(1, self.max_attempts + 1):
            timeout = self.timeout
            if self.deadline is not None:
                remaining = self.deadline - time.time()
                if remaining <= 0:
                    raise FnBudgetExhausted(
                        'fn budget exhausted before LLM attempt %d'
                        % attempt)
                timeout = max(1, min(timeout, int(remaining) + 1))
            try:
                with urllib.request.urlopen(request,
                                            timeout=timeout) as response:
                    payload = json.loads(response.read().decode('utf-8'))
                return self._extract(payload)
            except urllib.error.HTTPError as err:
                detail = self._http_detail(err)
                last_error = 'HTTP %d: %s' % (err.code, detail)
                if err.code in RETRYABLE_HTTP:
                    err.close()
                    if attempt < self.max_attempts:
                        self._sleep(attempt)
                        continue
                raise LLMError('LLM request failed (%s)' % last_error)
            except RETRYABLE_EXC as err:
                last_error = str(err) or err.__class__.__name__
                if attempt < self.max_attempts:
                    self._sleep(attempt)
                    continue
                raise LLMError('LLM endpoint unreachable after %d attempts'
                               ' (%s)' % (self.max_attempts, last_error))
        raise LLMError('LLM request failed (%s)' % last_error)

    @staticmethod
    def _extract(payload):
        """Pull (content, usage) out of a chat completions response."""
        choices = payload.get('choices') or []
        if not choices:
            raise LLMError('LLM response has no choices: %s'
                           % _truncate(json.dumps(payload)))
        message = choices[0].get('message') or {}
        content = message.get('content')
        if not isinstance(content, str) or not content.strip():
            raise LLMError('LLM response has no message content: %s'
                           % _truncate(json.dumps(payload)))
        return content, payload.get('usage') or {}

    @staticmethod
    def _http_detail(err):
        """First lines of an HTTPError body, for the error message."""
        try:
            detail = err.read().decode('utf-8', errors='replace')
        except Exception:  # noqa: BLE001 -- best effort only
            return '<no body>'
        return _truncate(detail)

    @staticmethod
    def _sleep(attempt):
        """Exponential backoff (2s, 4s, 8s) with jitter."""
        delay = (2 ** attempt) + random.uniform(0, 1)
        time.sleep(delay)


def _truncate(text, limit=400):
    """One line of at most `limit` characters."""
    collapsed = ' '.join(text.split())
    if len(collapsed) > limit:
        collapsed = collapsed[:limit] + '...'
    return collapsed
