package me.weishu.kernelsu.ui.screen.settings

import android.content.pm.ApplicationInfo
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import me.weishu.kernelsu.Natives
import me.weishu.kernelsu.R
import me.weishu.kernelsu.ui.util.updateDynamicManager

private data class ManagerCandidate(val name: String, val packages: String, val uid: Int, val granted: Boolean)

@Composable
fun DynamicManagersDialog(onDismiss: () -> Unit) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    var apps by remember { mutableStateOf<List<ManagerCandidate>>(emptyList()) }
    var query by remember { mutableStateOf("") }
    var error by remember { mutableStateOf<String?>(null) }
    var busy by remember { mutableStateOf(true) }
    var revision by remember { mutableIntStateOf(0) }
    var pending by remember { mutableStateOf<ManagerCandidate?>(null) }

    LaunchedEffect(revision) {
        busy = true
        try {
            apps = withContext(Dispatchers.IO) {
                val raw = checkNotNull(Natives.getDynamicManagers()) { "Cannot query dynamic managers" }
                val flags = raw.toList().chunked(2).associate { it[0] to it[1] }
                val pm = context.packageManager
                @Suppress("DEPRECATION")
                val installed = pm.getInstalledApplications(0)
                installed.filter { it.packageName != context.packageName && it.uid % 100000 >= 10000 &&
                    (it.flags and ApplicationInfo.FLAG_SYSTEM == 0 || flags.containsKey(it.uid % 100000)) }
                    .groupBy { it.uid }.map { (uid, shared) ->
                        ManagerCandidate(
                            shared.joinToString(" / ") { it.loadLabel(pm).toString() },
                            shared.joinToString("\n") { it.packageName }, uid,
                            (flags[uid % 100000] ?: 0) and Natives.DYNAMIC_MANAGER_GRANTED != 0
                        )
                    }.sortedWith(compareByDescending<ManagerCandidate> { it.granted }.thenBy { it.name.lowercase() })
            }
        } catch (exception: CancellationException) {
            throw exception
        } catch (exception: Exception) {
            error = exception.message
        } finally {
            busy = false
        }
    }

    Dialog(onDismissRequest = { if (!busy) onDismiss() },
        properties = DialogProperties(usePlatformDefaultWidth = false)) {
        Surface(modifier = Modifier.fillMaxSize()) {
            Column(Modifier.safeDrawingPadding().padding(16.dp)) {
                Text(stringResource(R.string.dynamic_managers), style = MaterialTheme.typography.headlineSmall)
                Text(stringResource(R.string.dynamic_managers_summary), modifier = Modifier.padding(vertical = 8.dp))
                OutlinedTextField(value = query, onValueChange = { query = it },
                    label = { Text(stringResource(R.string.dynamic_search)) }, singleLine = true,
                    modifier = Modifier.fillMaxWidth())
                error?.let { Text(it, color = MaterialTheme.colorScheme.error, modifier = Modifier.padding(vertical = 8.dp)) }
                if (busy) LinearProgressIndicator(modifier = Modifier.fillMaxWidth())
                LazyColumn(modifier = Modifier.weight(1f)) {
                    items(apps.filter { it.name.contains(query, true) || it.packages.contains(query, true) }, key = { it.uid }) { app ->
                        ListItem(
                            headlineContent = { Text(app.name) },
                            supportingContent = { Text("${app.packages}\nUID: ${app.uid}") },
                            trailingContent = {
                                Switch(checked = app.granted, enabled = !busy,
                                    onCheckedChange = { pending = app })
                            }
                        )
                    }
                }
                TextButton(onClick = onDismiss, enabled = !busy, modifier = Modifier.fillMaxWidth()) {
                    Text(stringResource(R.string.dynamic_close))
                }
            }
        }
    }
    pending?.let { app ->
        AlertDialog(
            onDismissRequest = { pending = null },
            title = { Text(stringResource(if (app.granted) R.string.dynamic_revoke else R.string.dynamic_authorize)) },
            text = { Text(stringResource(if (app.granted) R.string.dynamic_revoke_message else R.string.dynamic_authorize_message,
                app.name, app.packages)) },
            confirmButton = {
                TextButton(onClick = {
                    pending = null
                    busy = true
                    error = null
                    scope.launch {
                        updateDynamicManager(app.uid, !app.granted).onFailure { error = it.message }
                        revision++
                    }
                }) { Text(stringResource(R.string.dynamic_confirm)) }
            },
            dismissButton = { TextButton(onClick = { pending = null }) { Text(stringResource(R.string.dynamic_cancel)) } }
        )
    }
}
