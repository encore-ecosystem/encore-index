// Only clipboard enhancement; navigation and commands work without JavaScript.
document.querySelectorAll('[data-copy-package]').forEach(button => {
  button.hidden = false;
  let reset;
  button.addEventListener('click', async () => {
    const command = `encore add ${button.dataset.copyPackage}`;
    const status = document.getElementById('copy-status');
    clearTimeout(reset);
    try {
      await navigator.clipboard.writeText(command);
      button.dataset.copied = 'true';
      button.setAttribute('aria-label', `Copied: ${command}`);
      button.title = 'Copied!';
      status.classList.add('sr-only');
      status.textContent = `Copied: ${command}`;
      reset = setTimeout(() => {
        delete button.dataset.copied;
        button.setAttribute('aria-label', `Copy ${command}`);
        button.title = `Copy ${command}`;
      }, 2000);
    } catch {
      delete button.dataset.copied;
      button.setAttribute('aria-label', `Copy ${command}`);
      button.title = `Copy manually: ${command}`;
      status.textContent = `Clipboard unavailable. Copy manually: ${command}`;
      status.classList.remove('sr-only');
    }
  });
});
